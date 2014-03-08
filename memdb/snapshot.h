#pragma once

#include <inttypes.h>
#include <map>
#include <set>

#include "utils.h"

namespace mdb {

typedef int64_t version_t;

template <class Value>
struct versioned_value {
    version_t created_at, removed_at;

    // const value, not modifiable
    const Value val;

    versioned_value(version_t created, const Value& v): created_at(created), removed_at(-1), val(v) {}
    bool valid_at(version_t v) const {
        return created_at <= v && (removed_at == -1 || v < removed_at);
    }
    bool invalid_at_and_before(version_t v) const {
        return v < created_at;
    }
    bool invalid_at_and_after(version_t v) const {
        return removed_at >= 0 && removed_at <= v;
    }
    void remove(version_t v) {
        verify(removed_at == -1);
        removed_at = v;
        verify(created_at < removed_at);
    }
};

template <class Key, class Value, class Iterator, class Snapshot>
class snapshot_range: public Enumerator<std::pair<const Key&, const Value&>> {
    Snapshot snapshot_;
    Iterator begin_, end_, next_;
    bool cached_;
    std::pair<const Key*, const Value*> cached_next_;
    int count_;

    bool prefetch_next() {
        verify(cached_ == false);
        while (cached_ == false && next_ != end_) {
            if (next_->second.valid_at(snapshot_.version())) {
                cached_next_.first = &(next_->first);
                cached_next_.second = &(next_->second.val);
                cached_ = true;
            }
            ++next_;
        }
        return cached_;
    }

public:

    snapshot_range(const Snapshot& snapshot, Iterator it_begin, Iterator it_end)
        : snapshot_(snapshot), begin_(it_begin), end_(it_end), next_(it_begin), cached_(false), count_(-1) {}

    bool has_next() {
        if (cached_) {
            return true;
        } else {
            return prefetch_next();
        }
    }

    std::pair<const Key&, const Value&> next() {
        if (!cached_) {
            verify(prefetch_next());
        }
        cached_ = false;
        return std::pair<const Key&, const Value&>(*cached_next_.first, *cached_next_.second);
    }

    int count() {
        if (count_ >= 0) {
            return count_;
        }
        count_ = 0;
        for (auto it = begin_; it != end_; ++it) {
            if (it->second.valid_at(snapshot_.version())) {
                count_++;
            }
        }
        return count_;
    }

};

// A group of snapshots. Each snapshot in the group points to it, so they can share data.
// There could be at most one writer in the group. Members are ordered in a doubly linked list:
// S1 <= S2 <= S3 <= ... <= Sw (increasing version, writer at tail if exists)
template <class Key, class Value, class Container, class Snapshot>
struct snapshot_group: public RefCounted {
    Container data;
    std::multimap<version_t, std::pair<Key, Key>> removed_key_ranges;

    // the writer of the group, nullptr means nobody can write to the group
    Snapshot* writer;

    // TODO remove it, let Snapshot manage doubly linked list among them
    std::set<Snapshot*> snapshots;

    snapshot_group(Snapshot* w): writer(w) {}

    // protected dtor as required by RefCounted
protected:
    ~snapshot_group() {}
};

template <class Key, class Value>
class snapshot_sortedmap {

    // empty struct, used to mark a ctor as snapshotting
    struct snapshot_marker {};

public:

    typedef snapshot_range<
        Key,
        Value,
        typename std::multimap<Key, versioned_value<Value>>::const_iterator,
        snapshot_sortedmap> range_type;

    typedef snapshot_group<
        Key,
        Value,
        typename std::multimap<Key, versioned_value<Value>>,
        snapshot_sortedmap> snapshot_group;

    typedef typename std::pair<const Key&, const Value&> value_type;

private:

    version_t ver_;
    snapshot_group* ssg_;

    void make_me_snapshot_of(const snapshot_sortedmap& src) {
        verify(ver_ < 0);
        ver_ = src.ver_;
        verify(ssg_ == nullptr);
        ssg_ = (snapshot_group *) src.ssg_->ref_copy();
        ssg_->snapshots.insert(this);
    }

    void destory_me() {
        collect_my_garbage();

        if (ssg_->writer == this) {
            ssg_->writer = nullptr;
        } else {
            ssg_->snapshots.erase(this);
        }

        ssg_->release();
        ssg_ = nullptr;
        ver_ = -1;
    }

    // creating a snapshot
    snapshot_sortedmap(const snapshot_sortedmap& src, const snapshot_marker&): ver_(-1), ssg_(nullptr) {
        make_me_snapshot_of(src);
    }

public:

    // creating a new snapshot_sortedmap
    snapshot_sortedmap(): ver_(0) {
        ssg_ = new snapshot_group(this);
    }

    snapshot_sortedmap(const snapshot_sortedmap& src): ver_(-1), ssg_(nullptr) {
        if (src.readonly()) {
            // src is a snapshot, make me a snapshot, too
            ver_ = -1;
            ssg_ = nullptr;
            make_me_snapshot_of(src);
        } else {
            ver_ = 0;
            ssg_ = new snapshot_group(this);
            insert(src.all());
        }
    }

    template <class Iterator>
    snapshot_sortedmap(Iterator it_begin, Iterator it_end): ver_(0) {
        ssg_ = new snapshot_group(this);
        insert(it_begin, it_end);
    }

    ~snapshot_sortedmap() {
        destory_me();
    }

    version_t version() const {
        return ver_;
    }

    bool readonly() const {
        verify(this != nullptr);
        return ssg_->writer != this;
    }

    const snapshot_sortedmap& operator= (const snapshot_sortedmap& src) {
        if (&src != this) {
            destory_me();
            if (src.readonly()) {
                make_me_snapshot_of(src);
            } else {
                verify(ver_ == -1);
                verify(ssg_ == nullptr);
                ver_ = 0;
                ssg_ = new snapshot_group(this);
                insert(src.all());
            }
        }
        return *this;
    }

    // snapshot: readonly
    bool has_readonly_snapshot() const {
        return !ssg_->snapshots.empty();
    }

    bool has_writable_snapshot() const {
        return ssg_->writer != nullptr;
    }

    snapshot_sortedmap snapshot() const {
        return snapshot_sortedmap(*this, snapshot_marker());
    }

    const std::set<snapshot_sortedmap*>& all_snapshots() const {
        return this->ssg_->snapshots;
    }

    void insert(const Key& key, const Value& value) {
        verify(!readonly());
        ver_++;
        versioned_value<Value> vv(ver_, value);
        insert_into_map(ssg_->data, key, vv);
    }

    void insert(const value_type& kv_pair) {
        verify(!readonly());
        ver_++;
        versioned_value<Value> vv(ver_, kv_pair.second);
        insert_into_map(ssg_->data, kv_pair.first, vv);
    }

    template <class Iterator>
    void insert(Iterator begin, Iterator end) {
        verify(!readonly());
        ver_++;
        while (begin != end) {
            versioned_value<Value> vv(ver_, begin->second);
            insert_into_map(ssg_->data, begin->first, vv);
            ++begin;
        }
    }

    void insert(range_type range) {
        verify(!readonly());
        ver_++;
        while (range) {
            value_type kv_pair = range.next();
            versioned_value<Value> vv(ver_, kv_pair.second);
            insert_into_map(ssg_->data, kv_pair.first, vv);
        }
    }

    void erase(const Key& key) {
        verify(!readonly());
        ver_++;
        if (has_readonly_snapshot()) {
            for (auto it = ssg_->data.lower_bound(key); it != ssg_->data.upper_bound(key); ++it) {
                verify(key == it->first);
                it->second.remove(ver_);
            }
            insert_into_map(ssg_->removed_key_ranges, ver_, std::make_pair(key, key));
        } else {
            auto it = ssg_->data.lower_bound(key);
            while (it != ssg_->data.upper_bound(key)) {
                it = ssg_->data.erase(it);
            }
        }
    }

    range_type all() const {
        return range_type(this->snapshot(), this->ssg_->data.begin(), this->ssg_->data.end());
    }

    range_type query(const Key& key) const {
        return range_type(this->snapshot(), this->ssg_->data.lower_bound(key), this->ssg_->data.upper_bound(key));
    }

    range_type query_lt(const Key& key) const {
        return range_type(this->snapshot(), this->ssg_->data.begin(), this->ssg_->data.lower_bound(key));
    }

    range_type query_gt(const Key& key) const {
        return range_type(this->snapshot(), this->ssg_->data.upper_bound(key), this->ssg_->data.end());
    }

    size_t debug_storage_size() const {
        return this->ssg_->data.size();
    }

private:

    void gc_last_snapshot() {
        // do nothing, let dtor take over
    }

    void collect_my_garbage() {
        // TODO case by case GC, write gc_XYZ() functions
        verify(ver_ >= 0);

        // TODO when removing a snapshot S, GC keys only visible to this snapshot
        // if S is writer, let S' be the snapshot with highest version, any key invalid_at_and_after(S') should be collected
        // if S is reader, let S1 < S < S2, keys created_at > S1, deleted_at <= S2 should be collected

        // handle the special case of writer being destroyed
        if (!this->readonly() && !all_snapshots().empty()) {
            version_t max_ver = -1;
            for (auto it: all_snapshots()) {
                if (max_ver < it->version()) {
                    max_ver = it->version();
                }
            }
            auto it = ssg_->data.begin();
            while (it != ssg_->data.end()) {
                // all future query will have version > next_smallest_ver
                if (it->second.invalid_at_and_before(max_ver)) {
                    it = ssg_->data.erase(it);
                } else {
                    ++it;
                }
            }
            return;
        }

        for (auto& it : all_snapshots()) {
            if (it != this && it->version() <= this->version()) {
                return;
            }
        }

        version_t next_smallest_ver = -1;
        for (auto& it : all_snapshots()) {
            if (it == this) {
                continue;
            }
            next_smallest_ver = it->version();
            break;
        }
        if (next_smallest_ver == -1) {
            next_smallest_ver = ver_ + 1;
        }

        // GC based on tracking removed keys
        auto it_key_range = ssg_->removed_key_ranges.begin();
        while (it_key_range != ssg_->removed_key_ranges.upper_bound(next_smallest_ver)) {

            const Key& low = it_key_range->second.first;
            const Key& high = it_key_range->second.second;
            verify(low <= high);

            auto it = ssg_->data.lower_bound(low);
            while (it != ssg_->data.upper_bound(high)) {
                // all future query will have version > next_smallest_ver
                if (it->second.invalid_at_and_after(next_smallest_ver)) {
                    it = ssg_->data.erase(it);
                } else {
                    ++it;
                }
            }
            it_key_range = ssg_->removed_key_ranges.erase(it_key_range);
        }
        return;
    }

};


} // namespace mdb
