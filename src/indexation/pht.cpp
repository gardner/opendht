#include "indexation/pht.h"
#include "rng.h"

namespace dht {
namespace indexation {

void Pht::Cache::insert(const Prefix& p) {
    size_t i = 0;
    auto now = clock::now();

    std::shared_ptr<Node> curr_node;

    while ((leaves_.size() > 0 and leaves_.begin()->first + NODE_EXPIRE_TIME < now) or leaves_.size() > MAX_ELEMENT)
        leaves_.erase(leaves_.begin());

    if (not (curr_node = root_.lock()) ) {
        /* Root does not exist, need to create one*/
        curr_node = std::make_shared<Node>();
        root_ = curr_node;
    }

    curr_node->last_reply = now;

    /* Iterate through all bit of the Blob */
    for ( i = 0; i < p.size_; i++ ) {

        /* According to the bit define which node is the next one */
        auto& next = ( p.isActiveBit(i) ) ? curr_node->right_child : curr_node->left_child;
        /**
         * If lock, node exists
         * else create it
         */
        if (auto n = next.lock()) {
            curr_node = std::move(n);
        } else {
            /* Create the next node if doesn't exist*/
            auto tmp_curr_node = std::make_shared<Node>();
            tmp_curr_node->parent = curr_node;
            next = tmp_curr_node;
            curr_node = std::move(tmp_curr_node);
        }

        curr_node->last_reply = now;
    }

    /* Insert the leaf (curr_node) into the multimap */
    leaves_.emplace(std::move(now), std::move(curr_node) );
}

int Pht::Cache::lookup(const Prefix& p) {
    int pos = -1;
    auto now = clock::now(), last_node_time = now;

    /* Before lookup remove the useless one [i.e. too old] */
    while ( leaves_.size() > 0 and  leaves_.begin()->first + NODE_EXPIRE_TIME < now ) {
        leaves_.erase(leaves_.begin());
    }

    auto next = root_;
    std::shared_ptr<Node> curr_node;

    while ( auto n = next.lock() ) {
        ++pos;
        /* Safe since pos is equal to 0 until here */
        if ( (unsigned) pos >= p.content_.size() * 8) break;

        curr_node = n;
        last_node_time = curr_node->last_reply;
        curr_node->last_reply = now;

        /* Get the Prefix bit by bit, starting from left */
        next = ( p.isActiveBit(pos) ) ? curr_node->right_child : curr_node->left_child;
    }

    if ( pos >= 0 ) {
        auto to_erase = leaves_.find(last_node_time);
        if ( to_erase != leaves_.end() )
            leaves_.erase( to_erase );

        leaves_.emplace( std::move(now), std::move(curr_node) );
    }

    return pos;
}

const ValueType IndexEntry::TYPE = ValueType::USER_DATA;
constexpr std::chrono::minutes Pht::Cache::NODE_EXPIRE_TIME;

void Pht::lookupStep(Prefix p, std::shared_ptr<int> lo, std::shared_ptr<int> hi,
        std::shared_ptr<std::vector<std::shared_ptr<IndexEntry>>> vals,
        LookupCallbackWrapper cb, DoneCallbackSimple done_cb,
        std::shared_ptr<unsigned> max_common_prefix_len,
        int start, bool all_values)
{
    struct node_lookup_result {
        bool done {false};
        bool is_pht {false};
    };

    /* start could be under 0 but after the compare it to 0 it always will be unsigned, so we can cast it*/
    auto mid = (start >= 0) ? (unsigned) start : (*lo + *hi)/2;
    auto first_res = std::make_shared<node_lookup_result>();
    auto second_res = std::make_shared<node_lookup_result>();

    auto on_done = [=](bool ok) {
        bool is_leaf = first_res->is_pht and not second_res->is_pht;
        if (not ok) {
            if (done_cb)
                done_cb(false);
        }
        else if (is_leaf or *lo > *hi) {
            // leaf node
            Prefix to_insert = p.getPrefix(mid);

            if (cb) {
                if (vals->size() == 0 and max_common_prefix_len and mid > 0) {
                    auto p_ = (p.getPrefix(mid)).getSibling().getFullSize();
                    *lo = mid;
                    *hi = p_.size_;
                    lookupStep(p_, lo, hi, vals, cb, done_cb, max_common_prefix_len, -1, all_values);
                }

                cb(*vals, to_insert);
            }

            if (done_cb)
                done_cb(true);
        } else if (first_res->is_pht) {
            // internal node
            *lo = mid+1;
            lookupStep(p, lo, hi, vals, cb, done_cb, max_common_prefix_len, -1, all_values);
        } else {
            // first get failed before second.
            if (done_cb)
                done_cb(false);
        }
    };

    if (*lo <= *hi) {
        auto pht_filter = [&](const dht::Value& v) {
            return v.user_type.compare(0, name_.size(), name_) == 0;
        };

        auto on_get = [=](const std::shared_ptr<dht::Value>& value, std::shared_ptr<node_lookup_result> res) {
            if (value->user_type == canary_) {
                res->is_pht = true;
            }
            else {
                IndexEntry entry;
                entry.unpackValue(*value);

                if (max_common_prefix_len) { /* inexact match case */
                    auto common_bits = Prefix::commonBits(p, entry.prefix);
                    if (vals->empty()) {
                        vals->emplace_back(std::make_shared<IndexEntry>(entry));
                        *max_common_prefix_len = common_bits;
                    }
                    else {
                        if (common_bits == *max_common_prefix_len) /* this is the max so far */
                            vals->emplace_back(std::make_shared<IndexEntry>(entry));
                        else if (common_bits > *max_common_prefix_len) { /* new max found! */
                            vals->clear();
                            vals->emplace_back(std::make_shared<IndexEntry>(entry));
                            *max_common_prefix_len = common_bits;
                        }
                    }
                } else if (all_values or entry.prefix == p.content_) /* exact match case */
                    vals->emplace_back(std::make_shared<IndexEntry>(entry));
            }
            return true;
        };

        dht_->get(p.getPrefix(mid).hash(),
                std::bind(on_get, std::placeholders::_1, first_res),
                [=](bool ok) {
                    if (not ok) {
                        // DHT failed
                        first_res->done = true;
                        if (done_cb and second_res->done)
                            on_done(false);
                    }
                    else {
                        if (not first_res->is_pht) {
                            // Not a PHT node.
                            *hi = mid-1;
                            lookupStep(p, lo, hi, vals, cb, done_cb, max_common_prefix_len, -1, all_values);
                        } else {
                            first_res->done = true;
                            if (second_res->done or mid >= p.size_ - 1)
                                on_done(true);
                        }
                    }
                }, pht_filter);

        if (mid < p.size_ - 1)
           dht_->get(p.getPrefix(mid+1).hash(),
                    std::bind(on_get, std::placeholders::_1, second_res),
                    [=](bool ok) {
                        if (not ok) {
                            // DHT failed
                            second_res->done = true;
                            if (done_cb and first_res->done)
                                on_done(false);
                        }
                        else {
                            second_res->done = true;
                            if (first_res->done)
                                on_done(true);
                        }
                    }, pht_filter);

    } else {
        on_done(true);
    }
}

void Pht::lookup(Key k, Pht::LookupCallback cb, DoneCallbackSimple done_cb, bool exact_match) {
    auto prefix = linearize(k);
    auto values = std::make_shared<std::vector<std::shared_ptr<IndexEntry>>>();

    auto lo = std::make_shared<int>(0);
    auto hi = std::make_shared<int>(prefix.size_);
    std::shared_ptr<unsigned> max_common_prefix_len = not exact_match ? std::make_shared<unsigned>(0) : nullptr;

    lookupStep(prefix, lo, hi, values,
        [=](std::vector<std::shared_ptr<IndexEntry>>& entries, Prefix p) {
            std::vector<std::shared_ptr<Value>> vals(entries.size());

            std::transform(entries.begin(), entries.end(), vals.begin(),
                [](const std::shared_ptr<IndexEntry>& ie) {
                    return std::make_shared<Value>(ie->value);
            });

            cb(vals, p);
        }, done_cb, max_common_prefix_len, cache_.lookup(prefix));
}

void Pht::updateCanary(Prefix p) {
    // TODO: change this... copy value
    dht::Value canary_value;
    canary_value.user_type = canary_;

    dht_->put(p.hash(), std::move(canary_value),
        [=](bool){
            static std::bernoulli_distribution d(0.5);
            crypto::random_device rd;
            if (p.size_ and d(rd))
                updateCanary(p.getPrefix(-1));
        }
    );

    if (p.size_) {
        dht::Value canary_second_value;
        canary_second_value.user_type = canary_;
        dht_->put(p.getSibling().hash(), std::move(canary_second_value));
    }
}

void Pht::insert(Prefix kp, IndexEntry entry, std::shared_ptr<int> lo, std::shared_ptr<int> hi, time_point time_p,
                 bool check_split, DoneCallbackSimple done_cb) {

    if (time_p + ValueType::USER_DATA.expiration < clock::now()) return;

    auto vals = std::make_shared<std::vector<std::shared_ptr<IndexEntry>>>();
    auto final_prefix = std::make_shared<Prefix>();

    lookupStep(kp, lo, hi, vals,
        [=](std::vector<std::shared_ptr<IndexEntry>>&, Prefix p) {
            *final_prefix = Prefix(p);
        },
        [=](bool ok){
            if (not ok) {
                if (done_cb)
                    done_cb(false);
            } else {

                RealInsertCallback real_insert = [=]( std::shared_ptr<Prefix> p, IndexEntry entry) {
                    updateCanary(*p);
                    checkPhtUpdate(*p, entry, time_p);
                    cache_.insert(*p);
                    dht_->put(p->hash(), std::move(entry), done_cb, time_p);
                };

                if ( not check_split or final_prefix->size_ == kp.size_ ) {
                    real_insert(final_prefix, std::move(entry));
                } else {
                    if ( vals->size() < MAX_NODE_ENTRY_COUNT )
                        getRealPrefix(final_prefix, std::move(entry), real_insert);
                    else
                        split(*final_prefix, vals, entry, real_insert);
                }
            }
        }, nullptr, cache_.lookup(kp), true
    );
}

Prefix Pht::linearize(Key k) const {
    if (not validKey(k)) { throw std::invalid_argument(INVALID_KEY); }

    Prefix p = Blob {k.begin()->second.begin(), k.begin()->second.end()};

    auto bit_loc = p.size_ + 1;
    for ( auto i = p.content_.size(); i < keySpec_.begin()->second + 1; i++ )
        p.content_.push_back(0);

    return p.swapBit(bit_loc);
};

void Pht::getRealPrefix(std::shared_ptr<Prefix> p, IndexEntry entry, RealInsertCallback end_cb ) {
    if ( p->size_ == 0 ) {
        end_cb(p, std::move(entry));
        return;
    }

    auto total = std::make_shared<unsigned int>(0); /* Will contains the total number of data on 3 nodes */
    auto ended = std::make_shared<unsigned int>(0); /* Just indicate how many have end */

    auto parent = std::make_shared<Prefix>(p->getPrefix(-1));
    auto sibling = std::make_shared<Prefix>(p->getSibling());

    auto pht_filter = [&](const dht::Value& v) {
        return v.user_type.compare(0, name_.size(), name_) == 0;
    };

    /* Lambda will count total number of data node */
    auto count = [=]( const std::shared_ptr<dht::Value> value ) {
        if ( value->user_type != canary_)
            (*total)++;

        return true;
    };

    auto on_done = [=] ( bool ) {
        (*ended)++;
        /* Only the last one do the CallBack*/
        if  ( *ended == 3 ) {
            if ( *total < MAX_NODE_ENTRY_COUNT )
                end_cb(parent, std::move(entry));
            else
                end_cb(p, std::move(entry));
        }
    };

    dht_->get(parent->hash(),
        count,
        on_done,
        pht_filter
    );

    dht_->get(p->hash(),
        count,
        on_done,
        pht_filter
    );

    dht_->get(sibling->hash(),
        count,
        on_done,
        pht_filter
    );
}

void Pht::checkPhtUpdate(Prefix p, IndexEntry entry, time_point time_p) {

    Prefix full = entry.prefix;
    if ( p.content_.size() * 8 >= full.content_.size() * 8 ) return;

    auto next_prefix = full.getPrefix( p.size_ + 1 );

    dht_->listen(next_prefix.hash(),
        [=](const std::shared_ptr<dht::Value> &value) {
            if (value->user_type == canary_) {
                insert(full, entry, std::make_shared<int>(0), std::make_shared<int>(full.size_), time_p, false, nullptr);

                /* Cancel listen since we found where we need to update*/
                return false;
            }

            return true;
        },
        [=](const dht::Value& v) {
            /* Filter value v thats start with the same name as ours */
            return v.user_type.compare(0, name_.size(), name_) == 0;
        }
    );
}

void Pht::split(Prefix insert, std::shared_ptr<std::vector<std::shared_ptr<IndexEntry>>> vals, IndexEntry entry, RealInsertCallback end_cb ) {
    auto full = Prefix(entry.prefix);

    auto loc = foundSplitLocation(full, vals);
    auto prefix_to_insert = std::make_shared<Prefix>(full.getPrefix(loc));

    for(;loc != insert.size_ - 1; loc--)
        updateCanary(full.getPrefix(loc));

    end_cb(prefix_to_insert, entry);
}

} /* indexation  */

} /* dht */
