#include "unittest/dummy_protocol.hpp"

#include "errors.hpp"
#include <boost/make_shared.hpp>

#include "arch/timing.hpp"
#include "concurrency/rwi_lock.hpp"
#include "concurrency/signal.hpp"

namespace unittest {

dummy_protocol_t::region_t dummy_protocol_t::region_t::empty() THROWS_NOTHING {
    return region_t();
}

dummy_protocol_t::region_t dummy_protocol_t::read_t::get_region() const {
    return keys;
}

dummy_protocol_t::read_t dummy_protocol_t::read_t::shard(region_t region) const {
    rassert(region_is_superset(get_region(), region));
    read_t r;
    r.keys = region_intersection(region, keys);
    return r;
}

dummy_protocol_t::read_response_t dummy_protocol_t::read_t::unshard(std::vector<read_response_t> resps, UNUSED temporary_cache_t *cache) const {
    rassert(cache != NULL);
    read_response_t combined;
    for (int i = 0; i < (int)resps.size(); i++) {
        for (std::map<std::string, std::string>::const_iterator it = resps[i].values.begin();
                it != resps[i].values.end(); it++) {
            rassert(keys.keys.count((*it).first) != 0);
            rassert(combined.values.count((*it).first) == 0);
            combined.values[(*it).first] = (*it).second;
        }
    }
    return combined;
}

dummy_protocol_t::region_t dummy_protocol_t::write_t::get_region() const {
    region_t region;
    for (std::map<std::string, std::string>::const_iterator it = values.begin();
            it != values.end(); it++) {
        region.keys.insert((*it).first);
    }
    return region;
}

dummy_protocol_t::write_t dummy_protocol_t::write_t::shard(region_t region) const {
    rassert(region_is_superset(get_region(), region));
    write_t w;
    for (std::map<std::string, std::string>::const_iterator it = values.begin();
            it != values.end(); it++) {
        if (region.keys.count((*it).first) != 0) {
            w.values[(*it).first] = (*it).second;
        }
    }
    return w;
}

dummy_protocol_t::write_response_t dummy_protocol_t::write_t::unshard(std::vector<write_response_t> resps, UNUSED temporary_cache_t *cache) const {
    rassert(cache != NULL);
    write_response_t combined;
    for (int i = 0; i < (int)resps.size(); i++) {
        for (std::map<std::string, std::string>::const_iterator it = resps[i].old_values.begin();
                it != resps[i].old_values.end(); it++) {
            rassert(values.find((*it).first) != values.end());
            rassert(combined.old_values.count((*it).first) == 0);
            combined.old_values[(*it).first] = (*it).second;
        }
    }
    return combined;
}

bool region_is_superset(dummy_protocol_t::region_t a, dummy_protocol_t::region_t b) {
    for (std::set<std::string>::const_iterator it = b.keys.begin(); it != b.keys.end(); it++) {
        if (a.keys.count(*it) == 0) {
            return false;
        }
    }
    return true;
}

dummy_protocol_t::region_t region_intersection(dummy_protocol_t::region_t a, dummy_protocol_t::region_t b) {
    dummy_protocol_t::region_t i;
    for (std::set<std::string>::const_iterator it = a.keys.begin(); it != a.keys.end(); it++) {
        if (b.keys.count(*it) != 0) {
            i.keys.insert(*it);
        }
    }
    return i;
}

dummy_protocol_t::region_t region_join(std::vector<dummy_protocol_t::region_t> vec) THROWS_ONLY(bad_join_exc_t, bad_region_exc_t) {
    dummy_protocol_t::region_t u;
    for (std::vector<dummy_protocol_t::region_t>::iterator it = vec.begin(); it != vec.end(); it++) {
        for (std::set<std::string>::iterator it2 = (*it).keys.begin(); it2 != (*it).keys.end(); it2++) {
            if (u.keys.count(*it2) != 0) throw bad_join_exc_t();
            u.keys.insert(*it2);
        }
    }
    return u;
}

bool operator==(dummy_protocol_t::region_t a, dummy_protocol_t::region_t b) {
    return a.keys == b.keys;
}

bool operator!=(dummy_protocol_t::region_t a, dummy_protocol_t::region_t b) {
    return !(a == b);
}

dummy_underlying_store_t::dummy_underlying_store_t(dummy_protocol_t::region_t r) : region(r), metadata(r, binary_blob_t()) {
    for (std::set<std::string>::iterator it = region.keys.begin();
            it != region.keys.end(); it++) {
        values[*it] = "";
        timestamps[*it] = state_timestamp_t::zero();
    }
}

/* The same class handles both read and write transactions because it's more
    convenient that way. */

class dummy_transaction_t : public store_view_t<dummy_protocol_t>::write_transaction_t {

public:
    dummy_transaction_t(dummy_store_view_t *v, bool isr, rwi_lock_t::read_acq_t *read_acq_in, rwi_lock_t::write_acq_t *write_acq_in) :
        view(v), valid(true), is_read(isr)
    {
        if (is_read) {
            swap(*read_acq_in, read_acq);
        } else {
            swap(*write_acq_in, write_acq);
        }
    }

    region_map_t<dummy_protocol_t, binary_blob_t> get_metadata(signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        rassert(valid);
        if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
        return view->parent->metadata.mask(view->get_region());
    }

    dummy_protocol_t::read_response_t read(const dummy_protocol_t::read_t &read, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        rassert(valid);
        valid = false;
        dummy_protocol_t::read_response_t resp;
        {
            /* Swap `read_acq` or `write_acq` into a temp variable so it gets
            released when we throw an exception or finish */
            rwi_lock_t::read_acq_t temp_read_acq;
            rwi_lock_t::write_acq_t temp_write_acq;
            if (is_read) {
                swap(temp_read_acq, read_acq);
            } else {
                swap(temp_write_acq, write_acq);
            }
            if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
            for (std::set<std::string>::iterator it = read.keys.keys.begin();
                    it != read.keys.keys.end(); it++) {
                rassert(view->get_region().keys.count(*it) != 0);
                resp.values[*it] = view->parent->values[*it];
            }
        }
        if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
        return resp;
    }

    void send_backfill(const region_map_t<dummy_protocol_t, state_timestamp_t> &start_point,
            const boost::function<void(dummy_protocol_t::backfill_chunk_t)> &chunk_sender,
            signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t)
    {
        rassert(valid);
        valid = false;
        /* Make a copy so we can sleep and still have the correct semantics */
        std::map<std::string, std::string> values_snapshot = view->parent->values;
        std::map<std::string, state_timestamp_t> timestamps_snapshot = view->parent->timestamps;
        {
            /* Swap `read_acq` or `write_acq` into a temp variable so it gets
            released as soon as this timeout is over or interrupted */
            rwi_lock_t::read_acq_t temp_read_acq;
            rwi_lock_t::write_acq_t temp_write_acq;
            if (is_read) {
                swap(temp_read_acq, read_acq);
            } else {
                swap(temp_write_acq, write_acq);
            }
            if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
        }
        if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
        std::vector<std::pair<dummy_protocol_t::region_t, state_timestamp_t> > pairs = start_point.get_as_pairs();
        for (int i = 0; i < (int)pairs.size(); i++) {
            for (std::set<std::string>::iterator it = pairs[i].first.keys.begin();
                    it != pairs[i].first.keys.end(); it++) {
                if (timestamps_snapshot[*it] > pairs[i].second) {
                    dummy_protocol_t::backfill_chunk_t chunk;
                    chunk.key = *it;
                    chunk.value = values_snapshot[*it];
                    chunk.timestamp = timestamps_snapshot[*it];
                    chunk_sender(chunk);
                }
                if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
            }
        }
    }

    void set_metadata(const region_map_t<dummy_protocol_t, binary_blob_t> &metadata) THROWS_NOTHING {
        rassert(valid);
        if (rng.randint(2) == 0) nap(rng.randint(10));
        std::vector<std::pair<dummy_protocol_t::region_t, binary_blob_t> > old_pairs = view->parent->metadata.get_as_pairs();
        dummy_protocol_t::region_t cutout = metadata.get_domain();
        std::vector<std::pair<dummy_protocol_t::region_t, binary_blob_t> > new_pairs = metadata.get_as_pairs();
        for (int i = 0; i < (int)old_pairs.size(); i++) {
            dummy_protocol_t::region_t r = old_pairs[i].first;
            for (std::set<std::string>::iterator it = cutout.keys.begin(); it != cutout.keys.end(); it++) {
                r.keys.erase(*it);
            }
            if (!r.keys.empty()) {
                new_pairs.push_back(std::make_pair(r, old_pairs[i].second));
            }
        }
        view->parent->metadata = region_map_t<dummy_protocol_t, binary_blob_t>(new_pairs);
    }

    dummy_protocol_t::write_response_t write(const dummy_protocol_t::write_t &write, transition_timestamp_t transition_timestamp) THROWS_NOTHING {
        rassert(valid);
        valid = false;
        dummy_protocol_t::write_response_t resp;
        {
            rwi_lock_t::read_acq_t temp_read_acq;
            rwi_lock_t::write_acq_t temp_write_acq;
            if (is_read) {
                swap(temp_read_acq, read_acq);
            } else {
                swap(temp_write_acq, write_acq);
            }
            if (rng.randint(2) == 0) nap(rng.randint(10));
            for (std::map<std::string, std::string>::const_iterator it = write.values.begin();
                    it != write.values.end(); it++) {
                resp.old_values[(*it).first] = view->parent->values[(*it).first];
                view->parent->values[(*it).first] = (*it).second;
                view->parent->timestamps[(*it).first] = transition_timestamp.timestamp_after();
            }
        }
        if (rng.randint(2) == 0) nap(rng.randint(10));
        return resp;
    }

    void receive_backfill(const dummy_protocol_t::backfill_chunk_t &chunk, signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
        rassert(view->get_region().keys.count(chunk.key) != 0);
        rassert(valid);
        valid = false;
        {
            rwi_lock_t::read_acq_t temp_read_acq;
            rwi_lock_t::write_acq_t temp_write_acq;
            if (is_read) {
                swap(temp_read_acq, read_acq);
            } else {
                swap(temp_write_acq, write_acq);
            }
            if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
            view->parent->values[chunk.key] = chunk.value;
            view->parent->timestamps[chunk.key] = chunk.timestamp;
            if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
        }
        if (rng.randint(2) == 0) nap(rng.randint(10), interruptor);
    }

private:
    /* For random timeouts */
    rng_t rng;

    dummy_store_view_t *view;

    bool valid;

    bool is_read;
    rwi_lock_t::read_acq_t read_acq;
    rwi_lock_t::write_acq_t write_acq;
};

dummy_store_view_t::dummy_store_view_t(dummy_underlying_store_t *p, dummy_protocol_t::region_t region) :
    store_view_t<dummy_protocol_t>(region), parent(p) { }

boost::shared_ptr<store_view_t<dummy_protocol_t>::read_transaction_t> dummy_store_view_t::begin_read_transaction(UNUSED signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    rwi_lock_t::read_acq_t acq(&read_write_lock);
    return boost::make_shared<dummy_transaction_t>(this, true, &acq, static_cast<rwi_lock_t::write_acq_t *>(NULL));
}

boost::shared_ptr<store_view_t<dummy_protocol_t>::write_transaction_t> dummy_store_view_t::begin_write_transaction(UNUSED signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    rwi_lock_t::write_acq_t acq(&read_write_lock);
    return boost::make_shared<dummy_transaction_t>(this, false, static_cast<rwi_lock_t::read_acq_t *>(NULL), &acq);
}

}   /* namespace unittest */
