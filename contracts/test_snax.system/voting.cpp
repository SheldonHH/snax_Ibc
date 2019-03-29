/**
 *  @file
 *  @copyright defined in snax/LICENSE.txt
 */
#include "test_snax.system.hpp"

#include <snaxlib/snax.hpp>
#include <snaxlib/crypto.h>
#include <snaxlib/print.hpp>
#include <snaxlib/datastream.hpp>
#include <snaxlib/serialize.hpp>
#include <snaxlib/multi_index.hpp>
#include <snaxlib/privileged.hpp>
#include <snaxlib/singleton.hpp>
#include <snaxlib/transaction.hpp>
#include <snax.token/snax.token.hpp>

#include <algorithm>
#include <cmath>

namespace snaxsystem {
   using snax::indexed_by;
   using snax::const_mem_fun;
   using snax::bytes;
   using snax::print;
   using snax::singleton;
   using snax::transaction;

   /**
    *  This method will create a producer_config and producer_info object for 'producer'
    *
    *  @pre producer is not already registered
    *  @pre producer to register is an account
    *  @pre authority of producer to register
    *
    */
   void system_contract::regproducer( const account_name producer, const snax::public_key& producer_key, const std::string& url, uint16_t location ) {
      snax_assert( url.size() < 512, "url too long" );
      snax_assert( producer_key != snax::public_key(), "public key should not be the default value" );
      require_auth( producer );

      auto prod = _producers.find( producer );
      const auto current_time = snax::time_point_sec(now());

      if ( prod != _producers.end() ) {
         _producers.modify( prod, producer, [&]( producer_info& info ){
               info.producer_key = producer_key;
               info.is_active    = true;
               info.url          = url;
               info.location     = location;
               info.last_block_time = block_timestamp(current_time);
               info.last_top_list_entry_time = block_timestamp(0);
            });
      } else {
         _producers.emplace( producer, [&]( producer_info& info ){
               info.owner         = producer;
               info.total_votes   = 0;
               info.producer_key  = producer_key;
               info.is_active     = true;
               info.url           = url;
               info.location      = location;
               info.last_block_time = block_timestamp(current_time);
               info.last_top_list_entry_time = block_timestamp(0);
         });
      }
   }

   void system_contract::unregprod( const account_name producer ) {
      require_auth( producer );

      const auto& prod = _producers.get( producer, "producer not found" );

      _producers.modify( prod, 0, [&]( producer_info& info ){
            info.deactivate();
      });
   }

   void system_contract::update_elected_producers( block_timestamp block_time ) {
      _gstate.last_producer_schedule_update = block_time;

      auto idx = _producers.get_index<N(prototalvote)>();

      std::vector< std::pair<snax::producer_key,uint16_t> > top_producers;
      top_producers.reserve(_gstate.top_producers_limit);

      for ( auto it = idx.cbegin(); it != idx.cend() && top_producers.size() < _gstate.top_producers_limit && 0 < it->total_votes; ++it ) {
         if (it->active()) {
             const bool previous_iteration_in_top_list =
                block_time
                    .slot
                - it->last_top_list_entry_time
                    .slot
                == 120;

             if (!previous_iteration_in_top_list) {
                 const auto prod = _producers.find(it->owner);
                 if (prod != _producers.end()) {
                     _producers.modify( prod, 0, [&]( producer_info& info ){
                           info.last_top_list_entry_time = block_time;
                           info.last_block_time = block_time;
                     });
                 }
             }

             if (
                 previous_iteration_in_top_list
                 &&
                 block_time
                    .slot
                 - it->last_block_time
                    .slot
                 >= 7200
             ) {
                 const auto prod = _producers.find(it->owner);
                 if (prod != _producers.end()) {
                     _producers.modify( prod, 0, [&]( producer_info& info ){
                           info.deactivate();
                     });
                 }
             } else {
                 const auto prod = _producers.find(it->owner);
                 if (prod != _producers.end()) {
                     _producers.modify( prod, 0, [&]( producer_info& info ){
                           info.last_top_list_entry_time = block_time;
                     });
                 }
                 top_producers.emplace_back( std::pair<snax::producer_key,uint16_t>({{it->owner, it->producer_key}, it->location}) );
             }
         }
      }

      if (top_producers.size() == 0)
         return;

      /// sort by producer name
      std::sort( top_producers.begin(), top_producers.end() );

      std::vector<snax::producer_key> producers;

      producers.reserve(top_producers.size());
      for( const auto& item : top_producers )
         producers.push_back(item.first);

      bytes packed_schedule = pack(producers);

      if( set_proposed_producers( packed_schedule.data(),  packed_schedule.size() ) >= 0 ) {
         _gstate.last_producer_schedule_size = static_cast<decltype(_gstate.last_producer_schedule_size)>( top_producers.size() );
      }
   }

   double stake2vote( int64_t staked ) {
      /// TODO subtract 2080 brings the large numbers closer to this decade
      double weight = int64_t( (now() - (block_timestamp::block_timestamp_epoch / 1000)) / (seconds_per_day * 7) )  / double( 52 );
      return double(staked) / 100 * std::pow( 2, weight );
   }

   /**
    *  @pre producers must be sorted from lowest to highest and must be registered and active
    *  @pre if proxy is set then no producers can be voted for
    *  @pre if proxy is set then proxy account must exist and be registered as a proxy
    *  @pre every listed producer or proxy must have been previously registered
    *  @pre voter must authorize this action
    *  @pre voter must have previously staked some SNAX for voting
    *  @pre voter->staked must be up to date
    *
    *  @post every producer previously voted for will have vote reduced by previous vote weight
    *  @post every producer newly voted for will have vote increased by new vote amount
    *  @post prior proxy will proxied_vote_weight decremented by previous vote weight
    *  @post new proxy will proxied_vote_weight incremented by new vote weight
    *
    *  If voting for a proxy, the producer votes will not change until the proxy updates their own vote.
    */
   void system_contract::voteproducer( const account_name voter_name, const account_name proxy, const std::vector<account_name>& producers ) {
      require_auth( voter_name );
      update_votes( voter_name, proxy, producers, true );
   }

   void system_contract::update_votes( const account_name voter_name, const account_name proxy, const std::vector<account_name>& producers, bool voting ) {
      //validate input
      if ( proxy ) {
         snax_assert( producers.size() == 0, "cannot vote for producers and proxy at same time" );
         snax_assert( voter_name != proxy, "cannot proxy to self" );
         require_recipient( proxy );
      } else {
         snax_assert( producers.size() <= 30, "attempt to vote for too many producers" );
         for (auto producer = producers.begin(); producer < producers.end(); producer++)
            for (auto producer_to_compare = std::next(producer); producer_to_compare < producers.end(); producer_to_compare++)
                snax_assert(*producer == 0 || *producer_to_compare != *producer, "producers must be unique");
      }

      auto voter = _voters.find(voter_name);
      snax_assert( voter != _voters.end(), "user must stake before they can vote" ); /// staking creates voter object
      snax_assert( !proxy || !voter->is_proxy, "account registered as a proxy is not allowed to use a proxy" );

      /**
       * The first time someone votes we calculate and set last_vote_weight, since they cannot unstake until
       * after total_activated_stake hits threshold, we can use last_vote_weight to determine that this is
       * their first vote and should consider their stake activated.
       */
      if( voter->last_vote_weight <= 0.0 ) {
         _gstate.total_activated_stake += voter->staked;
         if( _gstate.total_activated_stake >= min_activated_stake && _gstate.thresh_activated_stake_time == 0 ) {
            _gstate.thresh_activated_stake_time = current_time();
         }
      }

      auto new_vote_weight = stake2vote( voter->staked );
      if( voter->is_proxy ) {
         new_vote_weight += voter->proxied_vote_weight;
      }

      boost::container::flat_map<account_name, pair<double, bool /*new*/> > producer_deltas;
      if ( voter->last_vote_weight > 0 ) {
         if( voter->proxy ) {
            auto old_proxy = _voters.find( voter->proxy );
            snax_assert( old_proxy != _voters.end(), "old proxy not found" ); //data corruption
            _voters.modify( old_proxy, 0, [&]( auto& vp ) {
                  vp.proxied_vote_weight -= voter->last_vote_weight;
               });
            propagate_weight_change( *old_proxy );
         } else {
            uint8_t iter = 0;
            for( const auto& p : voter->producers ) {
                if (p != 0) {
                   auto& d = producer_deltas[p];
                   d.first -= system_contract::apply_vote_weight(voter_name, voter->last_vote_weight, iter);
                   d.second = false;
                }
                iter++;
            }
         }
      }

      if( proxy ) {
         auto new_proxy = _voters.find( proxy );
         snax_assert( new_proxy != _voters.end(), "invalid proxy specified" ); //if ( !voting ) { data corruption } else { wrong vote }
         snax_assert( !voting || new_proxy->is_proxy, "proxy not found" );
         if ( new_vote_weight >= 0 ) {
            _voters.modify( new_proxy, 0, [&]( auto& vp ) {
                  vp.proxied_vote_weight += new_vote_weight;
               });
            propagate_weight_change( *new_proxy );
         }
      } else {
         if( new_vote_weight >= 0 ) {
            uint8_t iter = 0;
            for( const auto& p : producers ) {
                if (p != 0) {
                    auto vote_weight = system_contract::apply_vote_weight(voter_name, new_vote_weight, iter);
                    auto& d = producer_deltas[p];
                    d.first += vote_weight;
                    d.second = true;
                }
                iter++;
            }
         }
      }

      for( const auto& pd : producer_deltas ) {
         auto pitr = _producers.find( pd.first );
         if( pitr != _producers.end() ) {
            snax_assert( !voting || pitr->active() || !pd.second.second /* not from new set */, "producer is not currently registered" );
            _producers.modify( pitr, 0, [&]( auto& p ) {
               p.total_votes += pd.second.first;
               if ( p.total_votes < 0 ) { // floating point arithmetics can give small negative numbers
                  p.total_votes = 0;
               }
               _gstate.total_producer_vote_weight += pd.second.first;
               //snax_assert( p.total_votes >= 0, "something bad happened" );
            });
         } else {
            snax_assert( !pd.second.second /* not from new set */, "producer is not registered" ); //data corruption
         }
      }

      _voters.modify( voter, 0, [&]( auto& av ) {
         av.last_vote_weight = new_vote_weight;
         av.producers = producers;
         av.proxy     = proxy;
      });
   }

   /**
    *  An account marked as a proxy can vote with the weight of other accounts which
    *  have selected it as a proxy. Other accounts must refresh their voteproducer to
    *  update the proxy's weight.
    *
    *  @param isproxy - true if proxy wishes to vote on behalf of others, false otherwise
    *  @pre proxy must have something staked (existing row in voters table)
    *  @pre new state must be different than current state
    */
   void system_contract::regproxy( const account_name proxy, bool isproxy ) {
      require_auth( proxy );

      auto pitr = _voters.find(proxy);
      if ( pitr != _voters.end() ) {
         snax_assert( isproxy != pitr->is_proxy, "action has no effect" );
         snax_assert( !isproxy || !pitr->proxy, "account that uses a proxy is not allowed to become a proxy" );
         _voters.modify( pitr, 0, [&]( auto& p ) {
               p.is_proxy = isproxy;
            });
         propagate_weight_change( *pitr );
      } else {
         _voters.emplace( proxy, [&]( auto& p ) {
               p.owner  = proxy;
               p.is_proxy = isproxy;
            });
      }
   }

   void system_contract::propagate_weight_change( const voter_info& voter ) {
      snax_assert( voter.proxy == 0 || !voter.is_proxy, "account registered as a proxy is not allowed to use a proxy" );
      double new_weight = stake2vote( voter.staked );
      if ( voter.is_proxy ) {
         new_weight += voter.proxied_vote_weight;
      }

      /// don't propagate small changes (1 ~= epsilon)
      if ( fabs( new_weight - voter.last_vote_weight ) > 1 )  {
         if ( voter.proxy ) {
            auto& proxy = _voters.get( voter.proxy, "proxy not found" ); //data corruption
            _voters.modify( proxy, 0, [&]( auto& p ) {
                  p.proxied_vote_weight += new_weight - voter.last_vote_weight;
               }
            );
            propagate_weight_change( proxy );
         } else {
            auto delta = new_weight - voter.last_vote_weight;
            uint8_t iter = 0;
            for ( auto acnt : voter.producers ) {
               if (acnt) {
                   auto& pitr = _producers.get( acnt, "producer not found" ); //data corruption
                   _producers.modify( pitr, 0, [&]( auto& p ) {
                         auto votes = system_contract::apply_vote_weight(voter.owner, delta, iter);
                         p.total_votes += votes;
                         _gstate.total_producer_vote_weight += votes;
                   });
               }
               iter++;
            }
         }
      }
      _voters.modify( voter, 0, [&]( auto& v ) {
            v.last_vote_weight = new_weight;
         }
      );
   }

} /// namespace snaxsystem
