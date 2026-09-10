// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node.
//
// RetiMesh Node is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// RetiMesh Node is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with RetiMesh Node. If not, see <https://www.gnu.org/licenses/>.


// microStore's iterator: stepping is free, reading is not.
//
// This suite is a tripwire, not a test of our own code. The path table is
// RNS::Persistence::NewPathTable = microStore::TypedStore<Bytes,
// DestinationEntry, PathStore>, and the walk in refreshSnapshots()
// (src/rns/RnsTransport.cpp) is built on the promise that ++ touches the
// in-memory index only and that * is the sole path to the filesystem. That
// promise is what makes a resumable row cursor safe: a cursor that resumes at
// position N must reach it for one read, not N. If it costs N, resuming is
// the O(n^2) trap and the cursor must not be adopted.
//
// The promise is not ours to keep. microStore is pinned in platformio.ini to
// the branch dobrevit/microStore#retimesh/combined, not to a commit, so it
// moves under us between builds. It has already moved the wrong way once:
// TypedStore's iterator loaded in its constructor and again in operator++,
// so a traversal paid a read for every entry it stepped over, whether the
// caller looked at it or not. Upstream a213413 ("Load the record on
// dereference, not on every step") is the fix; the library's own comment in
// TypedStore.h records what the old shape cost.
//
// So if this suite goes red, the branch has regressed and the row cursor in
// refreshSnapshots() has to be reconsidered before anything else is done
// about it. Nothing here is a workaround to be relaxed.
//
// The promise has two halves and both are held here, because a regression in
// either one costs the firmware the same read per step:
//
//   * TypedStore's half — that it does not dereference the store beneath it
//     while stepping — over a hand-written CountingStore. What that fake
//     models is BasicFileStore's I/O manners and nothing more: operator++
//     moves a position and reads nothing, operator* is the only thing that
//     reads a value, and a value already read at this position is not read
//     again. It is not a BasicFileStore in any other respect — the real
//     operator++ calls load_meta(), which repopulates key/timestamp/ttl from
//     the in-memory index and clears the value; begin() flushes the write
//     buffer first, and a flush writes bytes to the segment file; there is a
//     metadata-only operator->; and the real Entry carries timestamp and ttl
//     this Record has no field for. None of that reads or opens a file, and
//     reads and opens are the only I/O the meters below count, so the
//     accounting still holds.
//
//   * BasicFileStore's own half — that load_meta() really is "Zero disk I/O",
//     which is its own comment on itself in FileStore.h, and that load_value()
//     is the only path to the filesystem — over a real microStore::FileStore
//     on a RAM filesystem that counts opens and reads. This half is not
//     reachable through the fake by construction, and it is the half the
//     firmware actually pays for.
#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "Config.h"                       // SNAPSHOT_MAX_PATHS
#include <microStore/FileStore.h>
#include <microStore/TypedStore.h>

namespace {

// As many rows as refreshSnapshots() will ever render in one pass. Taken from
// the firmware's own constant rather than repeated here, so a retune of the
// budget cannot leave this suite measuring a table size the node never builds.
// Big enough that "one read" and "one read per step" are not the same number
// by accident.
constexpr size_t kRows = SNAPSHOT_MAX_PATHS;

// Two of the tests below read that constant as a table size rather than as a
// budget: the subset walk expects exactly kRows/4 reads, and the resume walk
// visits {kRows-1, kRows/2, 1, 0}, which are only distinct in-range positions
// while the table is comfortably longer than one row. Pinned here so that
// retuning the budget cannot turn this tripwire red for a reason that has
// nothing to do with microStore — a red here has to mean upstream regressed.
static_assert(kRows % 4 == 0 && kRows >= 8,
              "test_typed_store_iterator needs a row count divisible by 4 and "
              "well above 1. If SNAPSHOT_MAX_PATHS is retuned past that, give "
              "this suite its own row count instead of relaxing its numbers.");

std::string keyAt(size_t n)   { return "k" + std::to_string(n); }
std::string valueAt(size_t n) { return "v" + std::to_string(n); }

// ---------------------------------------------------------------------------
// Part one: TypedStore, over a store with BasicFileStore's iterator contract
// and a meter on it.
// ---------------------------------------------------------------------------
class CountingStore {
public:
  // What the store hands out on dereference: the raw, still-encoded record,
  // which is what BasicFileStore::Entry is as far as TypedStore can see.
  struct Record {
    std::vector<uint8_t> key;
    std::vector<uint8_t> value;
  };

  void add(const std::string& key, const std::string& value) {
    _rows.push_back({key, value});
  }

  // Values actually read out of the store. On the real thing this is the
  // filesystem open-seek-read; here it is the only place the row's bytes are
  // touched, which is the same claim.
  size_t reads() const { return _reads; }
  // Positions stepped over. Counted so a test that expects zero reads cannot
  // pass because the walk never moved.
  size_t steps() const { return _steps; }
  void resetMeter() { _reads = 0; _steps = 0; }

  class iterator {
  public:
    iterator(CountingStore* store, size_t pos) : _store(store), _pos(pos) {}

    // Index-only, as BasicFileStore::operator++ is: it calls load_meta(),
    // which is documented there as zero disk I/O.
    iterator& operator++() {
      ++_pos;
      _loaded = false;
      _store->_steps++;
      return *this;
    }

    bool operator==(const iterator& other) const { return _pos == other._pos; }
    bool operator!=(const iterator& other) const { return _pos != other._pos; }

    const Record& operator*() const {
      if (!_loaded) read();
      return _current;
    }

  private:
    void read() const {
      _loaded = true;
      // Counted before the range check, not after: the meter records that the
      // store was asked for a row, and a request past the last one is still a
      // request. That is what makes the end() case below fail on its read
      // count rather than on an out-of-range index.
      _store->_reads++;
      if (_pos >= _store->_rows.size()) {
        // end() is not dereferenceable. If TypedStore's guard ever moves back
        // out of load(), the walk arrives here with _pos one past the last
        // row; hand back an empty record so the test that exists to catch
        // that reports a failed assertion instead of indexing off the end.
        // Without this the native build aborts on the out-of-range subscript,
        // and everything Unity had printed before it goes with the buffer —
        // measured here as no output at all and status 134, which PlatformIO's
        // reader can only show as an unexplained signal (docs/development.md).
        _current.key.clear();
        _current.value.clear();
        return;
      }
      const Row& row = _store->_rows[_pos];
      _current.key.assign(row.key.begin(), row.key.end());
      _current.value.assign(row.value.begin(), row.value.end());
    }

    CountingStore*  _store;
    size_t          _pos;
    mutable bool    _loaded = false;
    mutable Record  _current;
  };

  iterator begin() { return iterator(this, 0); }
  iterator end()   { return iterator(this, _rows.size()); }

  bool   isValid() const { return true; }
  size_t size() const { return _rows.size(); }

private:
  struct Row { std::string key; std::string value; };

  std::vector<Row> _rows;
  size_t           _reads = 0;
  size_t           _steps = 0;
};

// The real TypedStore, over the fake. std::string on both sides because
// Codec<std::string> ships with the library and the property under test is
// the iterator's, not the codec's.
using Store = microStore::TypedStore<std::string, std::string, CountingStore>;

void fill(CountingStore& store, size_t rows) {
  for (size_t n = 0; n < rows; n++) store.add(keyAt(n), valueAt(n));
  store.resetMeter();
}

// ---------------------------------------------------------------------------
// Part two: a RAM filesystem with a meter, for the real BasicFileStore.
//
// Records live in shared buffers rather than in the map itself, so a handle
// the store still holds survives the file being removed from under it — which
// is what flash does, and what the store's own compaction relies on.
// ---------------------------------------------------------------------------
class RamDisk {
public:
  using Blob = std::shared_ptr<std::vector<uint8_t>>;

  Blob find(const std::string& name) {
    auto it = _files.find(name);
    return it == _files.end() ? Blob() : it->second;
  }
  Blob create(const std::string& name) {
    Blob blob = std::make_shared<std::vector<uint8_t>>();
    _files[name] = blob;
    return blob;
  }
  bool erase(const std::string& name) { return _files.erase(name) > 0; }
  bool exists(const std::string& name) const { return _files.count(name) > 0; }
  bool rename(const std::string& from, const std::string& to) {
    auto it = _files.find(from);
    if (it == _files.end()) return false;
    _files[to] = it->second;
    _files.erase(it);
    return true;
  }

  // Files opened. On LittleFS this is the expensive half of a record read —
  // every open resolves the path afresh — which is why it is metered apart
  // from the bytes.
  size_t opens() const { return _opens; }
  // Calls that took content off a file: both read() overloads and peek().
  size_t reads() const { return _reads; }
  void resetMeter() { _opens = 0; _reads = 0; }

  void countOpen() { _opens++; }
  void countRead() { _reads++; }

private:
  std::map<std::string, Blob> _files;
  size_t _opens = 0;
  size_t _reads = 0;
};

class RamFileImpl : public microStore::FileImpl {
public:
  RamFileImpl(RamDisk* disk, RamDisk::Blob blob, const std::string& name, bool append)
    : _disk(disk), _blob(blob), _name(name), _pos(append ? blob->size() : 0) {}

protected:
  const char* name() const override { return _name.c_str(); }
  size_t size() const override { return _blob->size(); }
  void close() override { _open = false; }

  int read() override {
    _disk->countRead();
    if (_pos >= _blob->size()) return EOF;
    return (*_blob)[_pos++];
  }

  size_t read(uint8_t* buffer, size_t size) override {
    _disk->countRead();
    if (_pos >= _blob->size()) return 0;
    const size_t avail = _blob->size() - _pos;
    const size_t n = size < avail ? size : avail;
    memcpy(buffer, _blob->data() + _pos, n);
    _pos += n;
    return n;
  }

  size_t write(uint8_t ch) override {
    if (_pos >= _blob->size()) _blob->resize(_pos + 1);
    (*_blob)[_pos++] = ch;
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (_blob->size() < _pos + size) _blob->resize(_pos + size);
    memcpy(_blob->data() + _pos, buffer, size);
    _pos += size;
    return size;
  }

  int available() override {
    return _pos >= _blob->size() ? 0 : (int)(_blob->size() - _pos);
  }

  int peek() override {
    _disk->countRead();
    if (_pos >= _blob->size()) return EOF;
    return (*_blob)[_pos];
  }

  size_t tell() override { return _pos; }

  long seek(uint32_t pos, microStore::SeekMode mode) override {
    switch (mode) {
      case microStore::SeekModeEnd: _pos = _blob->size() + pos; break;
      case microStore::SeekModeCur: _pos += pos; break;
      case microStore::SeekModeSet:
      default:                      _pos = pos; break;
    }
    return (long)_pos;
  }

  void flush() override {}
  bool isValid() const override { return _open; }

private:
  RamDisk*      _disk;
  RamDisk::Blob _blob;
  std::string   _name;
  size_t        _pos;
  bool          _open = true;
};

class RamFileSystemImpl : public microStore::FileSystemImpl {
public:
  explicit RamFileSystemImpl(RamDisk* disk) : _disk(disk) {}

protected:
  microStore::File open(const char* path, microStore::File::Mode mode,
                        const bool create = false) override {
    (void)create;
    _disk->countOpen();
    // The four modes FileStore actually asks for: ModeWrite truncates,
    // ModeAppend and ModeReadAppend start at the end, ModeRead starts at 0 and
    // fails if the file is not there.
    const bool truncate = (mode == microStore::File::ModeWrite);
    const bool append   = (mode == microStore::File::ModeAppend ||
                           mode == microStore::File::ModeReadAppend);

    RamDisk::Blob blob = _disk->find(path);
    if (!blob) {
      if (mode == microStore::File::ModeRead) return {};
      blob = _disk->create(path);
    } else if (truncate) {
      blob->clear();
    }
    return microStore::File(new RamFileImpl(_disk, blob, path, append));
  }

  bool exists(const char* path) override { return _disk->exists(path); }
  bool remove(const char* path) override { return _disk->erase(path); }
  bool rename(const char* from, const char* to) override { return _disk->rename(from, to); }
  bool mkdir(const char* path) override { (void)path; return true; }
  bool rmdir(const char* path) override { (void)path; return true; }
  bool isDirectory(const char* path) override { (void)path; return false; }

  std::list<std::string> listDirectory(const char* path,
                                       Callbacks::DirectoryListing callback = nullptr) override {
    (void)path; (void)callback;
    return {};
  }

  size_t storageSize() override { return 0; }
  size_t storageAvailable() override { return 0; }

private:
  RamDisk* _disk;
};

// A real microStore::FileStore on that disk, filled and metered from zero.
// The meter is reset after the fill, so what the puts and the init cost is not
// charged to the walk.
void fillFileStore(microStore::FileStore& store, microStore::FileSystem& fs,
                   RamDisk& disk, size_t rows) {
  TEST_ASSERT_TRUE(store.init(fs, "/paths"));
  for (size_t n = 0; n < rows; n++)
    TEST_ASSERT_TRUE(store.put(keyAt(n), valueAt(n)));
  TEST_ASSERT_EQUAL_size_t(rows, store.size());
  disk.resetMeter();
}

// The value a key was stored with: keys are "kN", values "vN", and the index
// is an unordered_map so which key sits at which position is not predictable.
std::string valueForKey(const std::string& key) { return "v" + key.substr(1); }

} // namespace

// A whole walk with no dereference reads nothing. This is the property the
// row budget spends: positions a pass has no use for are stepped over for
// free, so the budget goes entirely on records that are going to be looked at.
void test_stepping_without_dereferencing_reads_nothing() {
  CountingStore backing;
  fill(backing, kRows);
  Store store(backing);

  size_t stepped = 0;
  for (auto it = store.begin(); it != store.end(); ++it) stepped++;

  TEST_ASSERT_EQUAL_size_t(kRows, stepped);
  TEST_ASSERT_EQUAL_size_t(kRows, backing.steps());   // the walk really happened
  TEST_ASSERT_EQUAL_size_t(0, backing.reads());       // and cost nothing
}

// ...and a walk that does dereference pays once per position, no more.
void test_dereferencing_reads_once_per_position() {
  CountingStore backing;
  fill(backing, kRows);
  Store store(backing);

  size_t seen = 0;
  for (auto it = store.begin(); it != store.end(); ++it) {
    Store::Entry& e = *it;
    TEST_ASSERT_EQUAL_STRING(keyAt(seen).c_str(), e.key.c_str());
    TEST_ASSERT_EQUAL_STRING(valueAt(seen).c_str(), e.value.c_str());
    seen++;
  }

  TEST_ASSERT_EQUAL_size_t(kRows, seen);
  TEST_ASSERT_EQUAL_size_t(kRows, backing.reads());
}

// The loaded_ cache: asking twice at one position reads once. Stepping clears
// it, so the next position is read afresh rather than served the old row.
void test_re_dereferencing_the_same_position_does_not_read_again() {
  CountingStore backing;
  fill(backing, kRows);
  Store store(backing);

  auto it = store.begin();
  TEST_ASSERT_EQUAL_STRING(keyAt(0).c_str(), (*it).key.c_str());
  TEST_ASSERT_EQUAL_size_t(1, backing.reads());
  TEST_ASSERT_EQUAL_STRING(keyAt(0).c_str(), (*it).key.c_str());
  TEST_ASSERT_EQUAL_size_t(1, backing.reads());

  ++it;
  TEST_ASSERT_EQUAL_STRING(keyAt(1).c_str(), (*it).key.c_str());
  TEST_ASSERT_EQUAL_size_t(2, backing.reads());
}

// The one the row cursor rests on.
//
// A cursor that resumes at position N advances an iterator from begin() to N
// and dereferences there. If advancing read, that costs a read per step and
// resuming across a table costs the sum of every prefix — the O(n^2) trap,
// and the reason an earlier attempt at a cursor was abandoned. It must cost
// exactly one read: the row it actually wants.
void test_reaching_position_n_costs_one_read_not_n() {
  CountingStore backing;
  fill(backing, kRows);
  Store store(backing);

  // Last, a couple in between, then first: a cursor resumes wherever it left
  // off, so "one read" has to hold at any position, not just at begin(). The
  // far end goes first on purpose — Unity stops a test at its first failure,
  // and a regression here should report the whole prefix it read rather than
  // the single read it would report at position 0. Restoring the pre-a213413
  // shape by hand and running this file against it does exactly that: it
  // reports kRows reads where it expected none, one per step plus the
  // constructor's.
  const size_t targets[] = { kRows - 1, kRows / 2, 1, 0 };

  for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
    const size_t target = targets[t];
    backing.resetMeter();

    auto it = store.begin();
    for (size_t n = 0; n < target; n++) ++it;
    TEST_ASSERT_EQUAL_size_t(target, backing.steps());
    TEST_ASSERT_EQUAL_size_t(0, backing.reads());     // getting there was free

    Store::Entry& e = *it;
    TEST_ASSERT_EQUAL_size_t(1, backing.reads());     // not target + 1
    // And it is the row that position holds, so the count above is a real
    // read of the right record and not a walk that stayed at begin().
    TEST_ASSERT_EQUAL_STRING(keyAt(target).c_str(), e.key.c_str());
    TEST_ASSERT_EQUAL_STRING(valueAt(target).c_str(), e.value.c_str());
  }
}

// What refreshSnapshots() actually does: step the whole table, dereference
// only the positions this pass wants. The bill is the rows read, not the
// table walked.
void test_a_walk_that_reads_a_subset_pays_only_for_that_subset() {
  CountingStore backing;
  fill(backing, kRows);
  Store store(backing);

  // One position in four.
  const size_t kEvery = 4;
  size_t pos = 0, read = 0;
  for (auto it = store.begin(); it != store.end(); ++it, ++pos) {
    if (pos % kEvery != 0) continue;
    TEST_ASSERT_EQUAL_STRING(valueAt(pos).c_str(), (*it).value.c_str());
    read++;
  }

  TEST_ASSERT_EQUAL_size_t(kRows / kEvery, read);
  TEST_ASSERT_EQUAL_size_t(kRows, backing.steps());
  TEST_ASSERT_EQUAL_size_t(kRows / kEvery, backing.reads());
}

// end() is not dereferenceable, and TypedStore says so in load() rather than
// by refusing to load in the constructor — which is where the guard had to
// move when the load became lazy (a213413). If it ever moves back out, a walk
// that dereferences its terminator asks the store for a row past the last one.
void test_dereferencing_end_reads_nothing() {
  CountingStore backing;
  fill(backing, kRows);
  Store store(backing);

  auto it = store.end();
  Store::Entry& e = *it;
  TEST_ASSERT_EQUAL_size_t(0, backing.reads());
  TEST_ASSERT_TRUE(e.key.empty());
  TEST_ASSERT_TRUE(e.value.empty());
}

// An empty table is walked without reading, and without stepping into it.
void test_an_empty_store_walks_without_reading() {
  CountingStore backing;
  fill(backing, 0);
  Store store(backing);

  size_t stepped = 0;
  for (auto it = store.begin(); it != store.end(); ++it) stepped++;

  TEST_ASSERT_EQUAL_size_t(0, stepped);
  TEST_ASSERT_EQUAL_size_t(0, backing.reads());
}

// The other half, on the real BasicFileStore: stepping the whole table opens
// no file and reads no byte. This is the claim TypedStore's iterator forwards
// and the one the firmware's walk is actually billed for — a regression
// confined to BasicFileStore::operator++ would leave every test above green
// while the node read a record off the filesystem at every step.
void test_file_store_stepping_opens_and_reads_nothing() {
  RamDisk disk;                                   // outlives the store below
  microStore::FileSystem fs(new RamFileSystemImpl(&disk));
  microStore::FileStore store;
  fillFileStore(store, fs, disk, kRows);

  // operator-> is the metadata-only dereference, so collecting keys through it
  // proves the walk visited every distinct record rather than just counting
  // increments — and it must still cost nothing.
  std::set<std::string> keys;
  bool loadedWhileStepping = false;
  for (auto it = store.begin(); it != store.end(); ++it) {
    keys.insert(std::string(it->key.begin(), it->key.end()));
    if (!it->value.empty()) loadedWhileStepping = true;
  }

  // The meter is asserted before the flag, and reads before opens, so that a
  // regression reports what the walk cost rather than that a value was not
  // empty. Hand-regressing operator++ to load eagerly and running this file
  // against it fails here on the read count, at two reads per record the walk
  // lands on — a header and a value — where none was expected.
  TEST_ASSERT_EQUAL_size_t(kRows, keys.size());
  TEST_ASSERT_EQUAL_size_t(0, disk.reads());
  TEST_ASSERT_EQUAL_size_t(0, disk.opens());
  TEST_ASSERT_FALSE(loadedWhileStepping);
}

// ...and dereferencing is what pays. Reaching the far end of the table costs
// nothing; the dereference there opens the segment once and reads the record.
void test_file_store_dereferencing_is_the_only_read() {
  RamDisk disk;
  microStore::FileSystem fs(new RamFileSystemImpl(&disk));
  microStore::FileStore store;
  fillFileStore(store, fs, disk, kRows);

  auto it = store.begin();
  for (size_t n = 0; n < kRows - 1; n++) ++it;    // to the last position
  TEST_ASSERT_EQUAL_size_t(0, disk.reads());      // getting there was free
  TEST_ASSERT_EQUAL_size_t(0, disk.opens());

  const microStore::FileStore::Entry& e = *it;
  TEST_ASSERT_GREATER_THAN_size_t(0, disk.reads());
  // It really went to the filesystem, and once: not zero, which would mean the
  // meter is not wired to the read path and the assertions above prove nothing.
  TEST_ASSERT_EQUAL_size_t(1, disk.opens());

  const std::string key(e.key.begin(), e.key.end());
  const std::string value(e.value.begin(), e.value.end());
  TEST_ASSERT_EQUAL_STRING(valueForKey(key).c_str(), value.c_str());
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_stepping_without_dereferencing_reads_nothing);
  RUN_TEST(test_dereferencing_reads_once_per_position);
  RUN_TEST(test_re_dereferencing_the_same_position_does_not_read_again);
  RUN_TEST(test_reaching_position_n_costs_one_read_not_n);
  RUN_TEST(test_a_walk_that_reads_a_subset_pays_only_for_that_subset);
  RUN_TEST(test_dereferencing_end_reads_nothing);
  RUN_TEST(test_an_empty_store_walks_without_reading);
  RUN_TEST(test_file_store_stepping_opens_and_reads_nothing);
  RUN_TEST(test_file_store_dereferencing_is_the_only_read);
  return UNITY_END();
}
