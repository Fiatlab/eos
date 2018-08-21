/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#include <eosio/elasticsearch_plugin/elasticsearch_plugin.hpp>
#include <eosio/chain/eosio_contract.hpp>
#include <eosio/chain/config.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/transaction.hpp>
#include <eosio/chain/types.hpp>

#include <fc/io/json.hpp>
#include <fc/utf8.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

#include <boost/chrono.hpp>
#include <boost/format.hpp>
#include <boost/signals2/connection.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>

#include <queue>


#include "elasticsearch_client.hpp"
#include "mappings.hpp"


namespace eosio {

using chain::account_name;
using chain::action_name;
using chain::block_id_type;
using chain::permission_name;
using chain::transaction;
using chain::signed_transaction;
using chain::signed_block;
using chain::transaction_id_type;
using chain::packed_transaction;

static appbase::abstract_plugin& _elasticsearch_plugin = app().register_plugin<elasticsearch_plugin>();

struct filter_entry {
   name receiver;
   name action;
   name actor;
   std::tuple<name, name, name> key() const {
      return std::make_tuple(receiver, action, actor);
   }
   friend bool operator<( const filter_entry& a, const filter_entry& b ) {
      return a.key() < b.key();
   }
};

class elasticsearch_plugin_impl {
public:
   elasticsearch_plugin_impl();
   ~elasticsearch_plugin_impl();

   fc::optional<boost::signals2::scoped_connection> accepted_block_connection;
   fc::optional<boost::signals2::scoped_connection> irreversible_block_connection;
   fc::optional<boost::signals2::scoped_connection> accepted_transaction_connection;
   fc::optional<boost::signals2::scoped_connection> applied_transaction_connection;

   void consume_blocks();

   void accepted_block( const chain::block_state_ptr& );
   void applied_irreversible_block(const chain::block_state_ptr&);
   void accepted_transaction(const chain::transaction_metadata_ptr&);
   void applied_transaction(const chain::transaction_trace_ptr&);
   void process_accepted_transaction(const chain::transaction_metadata_ptr&);
   void _process_accepted_transaction(const chain::transaction_metadata_ptr&);
   void process_applied_transaction(const chain::transaction_trace_ptr&);
   void _process_applied_transaction(const chain::transaction_trace_ptr&);
   void process_accepted_block( const chain::block_state_ptr& );
   void _process_accepted_block( const chain::block_state_ptr& );
   void process_irreversible_block(const chain::block_state_ptr&);
   void _process_irreversible_block(const chain::block_state_ptr&);

   optional<abi_serializer> get_abi_serializer( account_name n );
   template<typename T> fc::variant to_variant_with_abi( const T& obj );
   bool search_abi_by_account(fc::variant &v, const std::string &name);
   void purge_abi_cache();

   bool add_action_trace( elasticlient::SameIndexBulkData& bulk_action_traces, const chain::action_trace& atrace,
                          bool executed, const std::chrono::milliseconds& now );

   void create_account( const name& name, std::chrono::milliseconds& now );
   bool find_account( fc::variant& v, const account_name& name );
   bool find_block( fc::variant& v, const std::string& id );

   void update_account(const chain::action& act);

   void add_pub_keys( const vector<chain::key_weight>& keys, const account_name& name,
                      const permission_name& permission, const std::chrono::milliseconds& now );
   void remove_pub_keys( const account_name& name, const permission_name& permission );
   void add_account_control( const vector<chain::permission_level_weight>& controlling_accounts,
                             const account_name& name, const permission_name& permission,
                             const std::chrono::milliseconds& now );
   void remove_account_control( const account_name& name, const permission_name& permission );

   /// @return true if act should be added to elasticsearch, false to skip it
   bool filter_include( const chain::action& act ) const;

   void init();
   void delete_index();

   template<typename Queue, typename Entry> void queue(Queue& queue, const Entry& e);

   bool configured{false};
   bool delete_index_on_startup{false};
   uint32_t start_block_num = 0;
   std::atomic_bool start_block_reached{false};

   bool filter_on_star = true;
   std::set<filter_entry> filter_on;
   std::set<filter_entry> filter_out;
   bool store_blocks = true;
   bool store_block_states = true;
   bool store_transactions = true;
   bool store_transaction_traces = true;
   bool store_action_traces = true;

   std::string index_name;
   std::shared_ptr<elasticsearch_client> elastic_client;

   size_t max_queue_size = 0;
   int queue_sleep_time = 0;
   size_t abi_cache_size = 0;
   std::deque<chain::transaction_metadata_ptr> transaction_metadata_queue;
   std::deque<chain::transaction_metadata_ptr> transaction_metadata_process_queue;
   std::deque<chain::transaction_trace_ptr> transaction_trace_queue;
   std::deque<chain::transaction_trace_ptr> transaction_trace_process_queue;
   std::deque<chain::block_state_ptr> block_state_queue;
   std::deque<chain::block_state_ptr> block_state_process_queue;
   std::deque<chain::block_state_ptr> irreversible_block_state_queue;
   std::deque<chain::block_state_ptr> irreversible_block_state_process_queue;
   boost::mutex mtx;
   boost::condition_variable condition;
   boost::thread consume_thread;
   boost::atomic<bool> done{false};
   boost::atomic<bool> startup{true};
   fc::optional<chain::chain_id_type> chain_id;
   fc::microseconds abi_serializer_max_time;

   struct by_account;
   struct by_last_access;

   struct abi_cache {
      account_name                     account;
      fc::time_point                   last_accessed;
      fc::optional<abi_serializer>     serializer;
   };

   typedef boost::multi_index_container<abi_cache,
         indexed_by<
               ordered_unique< tag<by_account>,  member<abi_cache,account_name,&abi_cache::account> >,
               ordered_non_unique< tag<by_last_access>,  member<abi_cache,fc::time_point,&abi_cache::last_accessed> >
         >
   > abi_cache_index_t;

   abi_cache_index_t abi_cache_index;

   static const action_name newaccount;
   static const action_name setabi;
   static const action_name updateauth;
   static const action_name deleteauth;
   static const permission_name owner;
   static const permission_name active;

   static const std::string block_states_type;
   static const std::string blocks_type;
   static const std::string trans_type;
   static const std::string trans_traces_type;
   static const std::string action_traces_type;
   static const std::string accounts_type;
   static const std::string pub_keys_type;
   static const std::string account_controls_type;
};

const action_name elasticsearch_plugin_impl::newaccount = chain::newaccount::get_name();
const action_name elasticsearch_plugin_impl::setabi = chain::setabi::get_name();
const action_name elasticsearch_plugin_impl::updateauth = chain::updateauth::get_name();
const action_name elasticsearch_plugin_impl::deleteauth = chain::deleteauth::get_name();
const permission_name elasticsearch_plugin_impl::owner = chain::config::owner_name;
const permission_name elasticsearch_plugin_impl::active = chain::config::active_name;

const std::string elasticsearch_plugin_impl::block_states_type = "block_states";
const std::string elasticsearch_plugin_impl::blocks_type = "blocks";
const std::string elasticsearch_plugin_impl::trans_type = "transactions";
const std::string elasticsearch_plugin_impl::trans_traces_type = "transaction_traces";
const std::string elasticsearch_plugin_impl::action_traces_type = "action_traces";
const std::string elasticsearch_plugin_impl::accounts_type = "accounts";
const std::string elasticsearch_plugin_impl::pub_keys_type = "pub_keys";
const std::string elasticsearch_plugin_impl::account_controls_type = "account_controls";

bool elasticsearch_plugin_impl::filter_include( const chain::action& act ) const {
   bool include = false;
   if( filter_on_star || filter_on.find( {act.account, act.name, 0} ) != filter_on.end() ) {
      include = true;
   } else {
      for( const auto& a : act.authorization ) {
         if( filter_on.find( {act.account, act.name, a.actor} ) != filter_on.end() ) {
            include = true;
            break;
         }
      }
   }

   if( !include ) { return false; }

   if( filter_out.find( {act.account, 0, 0} ) != filter_out.end() ) {
      return false;
   }
   if( filter_out.find( {act.account, act.name, 0} ) != filter_out.end() ) {
      return false;
   }
   for( const auto& a : act.authorization ) {
      if( filter_out.find( {act.account, act.name, a.actor} ) != filter_out.end() ) {
         return false;
      }
   }
   return true;
}

elasticsearch_plugin_impl::elasticsearch_plugin_impl()
{
}

elasticsearch_plugin_impl::~elasticsearch_plugin_impl()
{
   if (!startup) {
      try {
         ilog( "elasticsearch_plugin shutdown in process please be patient this can take a few minutes" );
         done = true;
         condition.notify_one();

         consume_thread.join();
      } catch( std::exception& e ) {
         elog( "Exception on elasticsearch_plugin shutdown of consume thread: ${e}", ("e", e.what()));
      }
   }
}

template<typename Queue, typename Entry>
void elasticsearch_plugin_impl::queue( Queue& queue, const Entry& e ) {
   boost::mutex::scoped_lock lock( mtx );
   auto queue_size = queue.size();
   if( queue_size > max_queue_size ) {
      lock.unlock();
      condition.notify_one();
      queue_sleep_time += 10;
      if( queue_sleep_time > 1000 )
         wlog("queue size: ${q}", ("q", queue_size));
      boost::this_thread::sleep_for( boost::chrono::milliseconds( queue_sleep_time ));
      lock.lock();
   } else {
      queue_sleep_time -= 10;
      if( queue_sleep_time < 0 ) queue_sleep_time = 0;
   }
   queue.emplace_back( e );
   lock.unlock();
   condition.notify_one();
}

void elasticsearch_plugin_impl::accepted_transaction( const chain::transaction_metadata_ptr& t ) {
   try {
      queue( transaction_metadata_queue, t );
   } catch (fc::exception& e) {
      elog("FC Exception while accepted_transaction ${e}", ("e", e.to_string()));
   } catch (std::exception& e) {
      elog("STD Exception while accepted_transaction ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while accepted_transaction");
   }
}

void elasticsearch_plugin_impl::applied_transaction( const chain::transaction_trace_ptr& t ) {
   try {
      queue( transaction_trace_queue, t );
   } catch (fc::exception& e) {
      elog("FC Exception while applied_transaction ${e}", ("e", e.to_string()));
   } catch (std::exception& e) {
      elog("STD Exception while applied_transaction ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while applied_transaction");
   }
}

void elasticsearch_plugin_impl::applied_irreversible_block( const chain::block_state_ptr& bs ) {
   try {
      queue( irreversible_block_state_queue, bs );
   } catch (fc::exception& e) {
      elog("FC Exception while applied_irreversible_block ${e}", ("e", e.to_string()));
   } catch (std::exception& e) {
      elog("STD Exception while applied_irreversible_block ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while applied_irreversible_block");
   }
}

void elasticsearch_plugin_impl::accepted_block( const chain::block_state_ptr& bs ) {
   try {
      queue( block_state_queue, bs );
   } catch (fc::exception& e) {
      elog("FC Exception while accepted_block ${e}", ("e", e.to_string()));
   } catch (std::exception& e) {
      elog("STD Exception while accepted_block ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while accepted_block");
   }
}

void elasticsearch_plugin_impl::purge_abi_cache() {
   if( abi_cache_index.size() < abi_cache_size ) return;

   // remove the oldest (smallest) last accessed
   auto& idx = abi_cache_index.get<by_last_access>();
   auto itr = idx.begin();
   if( itr != idx.end() ) {
      idx.erase( itr );
   }
}

bool elasticsearch_plugin_impl::search_abi_by_account(fc::variant &v, const std::string &name) {
   fc::variant res;
   if ( !find_account(res, name) ) return false;

   size_t pos = 0;
   try {
      v = res["_source"]["abi"];
   } catch( ... ) {
      return false;
   }

   return true;
}

optional<abi_serializer> elasticsearch_plugin_impl::get_abi_serializer( account_name n ) {
   if( n.good()) {
      try {

         auto itr = abi_cache_index.find( n );
         if( itr != abi_cache_index.end() ) {
            abi_cache_index.modify( itr, []( auto& entry ) {
               entry.last_accessed = fc::time_point::now();
            });

            return itr->serializer;
         }

         fc::variant abi_v;
         if(search_abi_by_account(abi_v, n.to_string())) {
            abi_def abi;
            try {
               abi = abi_v.as<abi_def>();
            } catch (...) {
               ilog( "Unable to convert account abi to abi_def for ${n}", ( "n", n ));
               return optional<abi_serializer>();
            }

            purge_abi_cache(); // make room if necessary
            abi_cache entry;
            entry.account = n;
            entry.last_accessed = fc::time_point::now();
            abi_serializer abis;
            if( n == chain::config::system_account_name ) {
               // redefine eosio setabi.abi from bytes to abi_def
               // Done so that abi is stored as abi_def in mongo instead of as bytes
               auto itr = std::find_if( abi.structs.begin(), abi.structs.end(),
                                          []( const auto& s ) { return s.name == "setabi"; } );
               if( itr != abi.structs.end() ) {
                  auto itr2 = std::find_if( itr->fields.begin(), itr->fields.end(),
                                             []( const auto& f ) { return f.name == "abi"; } );
                  if( itr2 != itr->fields.end() ) {
                     if( itr2->type == "bytes" ) {
                        itr2->type = "abi_def";
                        // unpack setabi.abi as abi_def instead of as bytes
                        abis.add_specialized_unpack_pack( "abi_def",
                              std::make_pair<abi_serializer::unpack_function, abi_serializer::pack_function>(
                                    []( fc::datastream<const char*>& stream, bool is_array, bool is_optional ) -> fc::variant {
                                       EOS_ASSERT( !is_array && !is_optional, chain::mongo_db_exception, "unexpected abi_def");
                                       chain::bytes temp;
                                       fc::raw::unpack( stream, temp );
                                       return fc::variant( fc::raw::unpack<abi_def>( temp ) );
                                    },
                                    []( const fc::variant& var, fc::datastream<char*>& ds, bool is_array, bool is_optional ) {
                                       EOS_ASSERT( false, chain::mongo_db_exception, "never called" );
                                    }
                              ) );
                     }
                  }
               }
            }
            abis.set_abi( abi, abi_serializer_max_time );
            entry.serializer.emplace( std::move( abis ) );
            abi_cache_index.insert( entry );
            return entry.serializer;
         }
      } FC_CAPTURE_AND_LOG((n))
   }
   return optional<abi_serializer>();
}

template<typename T>
fc::variant elasticsearch_plugin_impl::to_variant_with_abi( const T& obj ) {
   fc::variant pretty_output;
   abi_serializer::to_variant( obj, pretty_output,
                               [&]( account_name n ) { return get_abi_serializer( n ); },
                               abi_serializer_max_time );
   return pretty_output;
}


void elasticsearch_plugin_impl::process_accepted_transaction( const chain::transaction_metadata_ptr& t ) {
   try {
      // always call since we need to capture setabi on accounts even if not storing transactions
      _process_accepted_transaction(t);
   } catch (fc::exception& e) {
      elog("FC Exception while processing accepted transaction metadata: ${e}", ("e", e.to_detail_string()));
   } catch (std::exception& e) {
      elog("STD Exception while processing accepted tranasction metadata: ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while processing accepted transaction metadata");
   }
}

void elasticsearch_plugin_impl::process_applied_transaction( const chain::transaction_trace_ptr& t ) {
   try {
      if( start_block_reached ) {
         _process_applied_transaction( t );
      }
   } catch (fc::exception& e) {
      elog("FC Exception while processing applied transaction trace: ${e}", ("e", e.to_detail_string()));
   } catch (std::exception& e) {
      elog("STD Exception while processing applied transaction trace: ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while processing applied transaction trace");
   }
}

void elasticsearch_plugin_impl::process_irreversible_block(const chain::block_state_ptr& bs) {
  try {
     if( start_block_reached ) {
      //   _process_irreversible_block( bs );
     }
  } catch (fc::exception& e) {
     elog("FC Exception while processing irreversible block: ${e}", ("e", e.to_detail_string()));
  } catch (std::exception& e) {
     elog("STD Exception while processing irreversible block: ${e}", ("e", e.what()));
  } catch (...) {
     elog("Unknown exception while processing irreversible block");
  }
}

void elasticsearch_plugin_impl::process_accepted_block( const chain::block_state_ptr& bs ) {
   try {
      if( !start_block_reached ) {
         if( bs->block_num >= start_block_num ) {
            start_block_reached = true;
         }
      }
      if( start_block_reached ) {
         _process_accepted_block( bs ); 
      }
   } catch (fc::exception& e) {
      elog("FC Exception while processing accepted block trace ${e}", ("e", e.to_string()));
   } catch (std::exception& e) {
      elog("STD Exception while processing accepted block trace ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while processing accepted block trace");
   }
}


void handle_elasticsearch_exception( const std::string& desc, int line_num ) {
   bool shutdown = true;
   try {
      try {
         throw;
      } catch( elasticlient::ConnectionException& e) {
         elog( "elasticsearch connection error, ${desc}, line ${line}, ${what}",
               ("desc", desc)( "line", line_num )( "what", e.what() ));
      } catch( chain::response_code_exception& e) {
         elog( "elasticsearch exception, ${desc}, line ${line}, ${what}",
               ("desc", desc)( "line", line_num )( "what", e.what() ));
      } catch( chain::bulk_fail_exception& e) {
         elog( "elasticsearch exception, ${desc}, line ${line}, ${what}",
               ("desc", desc)( "line", line_num )( "what", e.what() ));
       } catch( fc::exception& er ) {
         elog( "elasticsearch fc exception, ${desc}, line ${line}, ${details}",
               ("desc", desc)( "line", line_num )( "details", er.to_detail_string()));
      } catch( const std::exception& e ) {
         elog( "elasticsearch std exception, ${desc}, line ${line}, ${what}",
               ("desc", desc)( "line", line_num )( "what", e.what()));
      } catch( ... ) {
         elog( "elasticsearch unknown exception, ${desc}, line ${line_nun}", ("desc", desc)( "line_num", line_num ));
      }
   } catch (...) {
      std::cerr << "Exception attempting to handle exception for " << desc << " " << line_num << std::endl;
   }

   if( shutdown ) {
      // shutdown if elasticsearch failed to provide opportunity to fix issue and restart
      app().quit();
   }
}


void elasticsearch_plugin_impl::add_pub_keys( const vector<chain::key_weight>& keys, const account_name& name,
                                         const permission_name& permission, const std::chrono::milliseconds& now )
{
   if( keys.empty()) return;

   elasticlient::SameIndexBulkData bulk_pub_keys(index_name);

   for( const auto& pub_key_weight : keys ) {
      fc::mutable_variant_object doc;
      doc["account"] = name.to_string();
      doc["public_key"] = pub_key_weight.key.operator string();
      doc["permission"] = permission.to_string();
      doc["createAt"] = now.count();
      auto json = fc::json::to_string( doc );
      bulk_pub_keys.indexDocument(pub_keys_type, "", json);
   }

   try {
      elastic_client->bulk_perform(bulk_pub_keys);
   } catch( ... ) {
      handle_elasticsearch_exception( "action traces", __LINE__ );
   }
}

void elasticsearch_plugin_impl::remove_pub_keys( const account_name& name, const permission_name& permission )
{
   auto query_pattern = R"(
{
  "query": {
    "bool": {
      "must": [
        {
          "term": {
            "account": "%1%"
          }
        },
        {
          "term": {
            "permission": "%2%"
          }
        }
      ]
    }
  }
}
)";
   try {
      auto query = boost::str(boost::format(query_pattern) % name.to_string() % permission.to_string());
      elastic_client->delete_by_query(pub_keys_type, query);
   } catch (...) {
      handle_elasticsearch_exception( "pub_keys delete", __LINE__ );
   }
}

void elasticsearch_plugin_impl::add_account_control( const vector<chain::permission_level_weight>& controlling_accounts,
                                                const account_name& name, const permission_name& permission,
                                                const std::chrono::milliseconds& now )
{
   if( controlling_accounts.empty()) return;

   elasticlient::SameIndexBulkData bulk_account_controls(index_name);

   for( const auto& controlling_account : controlling_accounts ) {
      fc::mutable_variant_object doc;
      doc["controlled_account"] = name.to_string();
      doc["controlled_permission"] = permission.to_string();
      doc["controlling_account"] = controlling_account.permission.actor.to_string();
      doc["createAt"] = now.count();
      auto json = fc::json::to_string( doc );
      bulk_account_controls.indexDocument( account_controls_type, "", json );
   }

   try {
      elastic_client->bulk_perform(bulk_account_controls);
   } catch( ... ) {
      handle_elasticsearch_exception( "account_controls bulk", __LINE__ );
   }
}

void elasticsearch_plugin_impl::remove_account_control( const account_name& name, const permission_name& permission )
{
   auto query_pattern = R"(
{
  "query": {
    "bool": {
      "must": [
        {
          "term": {
            "controlled_account": "%1%"
          }
        },
        {
          "term": {
            "controlled_permission": "%2%"
          }
        }
      ]
    }
  }
}
)";
   try {
      auto query = boost::str(boost::format(query_pattern) % name.to_string() % permission.to_string());
      elastic_client->delete_by_query(account_controls_type, query);
   } catch (...) {
      handle_elasticsearch_exception( "account_controls delete", __LINE__ );
   }
}

void elasticsearch_plugin_impl::create_account( const name& name, std::chrono::milliseconds& now )
{
   fc::mutable_variant_object account_doc;
   account_doc["name"] = name.to_string();
   account_doc["createAt"] = now.count();

   auto json = fc::json::to_string( account_doc );

   try {
      elastic_client->index(accounts_type, json);
   } catch( ... ) {
      handle_elasticsearch_exception( "create_account" + json, __LINE__ );
   }
}

bool elasticsearch_plugin_impl::find_account( fc::variant& v, const account_name& name )
{
   auto account_name = name.to_string();
   fc::variant res;
   std::string query = boost::str(boost::format(R"({"query" : { "term" : { "name" : "%1%" }}})") % account_name);
   elastic_client->search(res, accounts_type, query);

   if(res["hits"]["total"] != 1) return false;

   size_t pos = 0;
   v = res["hits"]["hits"][pos];

   return true;
}

bool elasticsearch_plugin_impl::find_block( fc::variant& v, const std::string& id )
{
   fc::variant res;
   std::string query = boost::str(boost::format(R"({"query" : { "term" : { "block_id" : "%1%" }}})") % id);
   elastic_client->search(res, blocks_type, query);

   if(res["hits"]["total"] != 1) return false;

   size_t pos = 0;
   v = res["hits"]["hits"][pos]["_source"];

   return true;
}


void elasticsearch_plugin_impl::update_account(const chain::action& act)
{
   if (act.account != chain::config::system_account_name)
      return;

   try {
      if( act.name == newaccount ) {
         std::chrono::milliseconds now = std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()} );
         auto newacc = act.data_as<chain::newaccount>();

         create_account( newacc.name, now );

         add_pub_keys( newacc.owner.keys, newacc.name, owner, now );
         add_account_control( newacc.owner.accounts, newacc.name, owner, now );
         add_pub_keys( newacc.active.keys, newacc.name, active, now );
         add_account_control( newacc.active.accounts, newacc.name, active, now );

      } else if( act.name == updateauth ) {
         auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()} );
         const auto update = act.data_as<chain::updateauth>();
         remove_pub_keys(update.account, update.permission);
         remove_account_control(update.account, update.permission);
         add_pub_keys(update.auth.keys, update.account, update.permission, now);
         add_account_control(update.auth.accounts, update.account, update.permission, now);

      } else if( act.name == deleteauth ) {
         const auto del = act.data_as<chain::deleteauth>();
         remove_pub_keys( del.account, del.permission );
         remove_account_control(del.account, del.permission);

      } else if( act.name == setabi ) {
         auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()} );
         auto setabi = act.data_as<chain::setabi>();

         abi_cache_index.erase( setabi.account );

         fc::variant account;

         if( !find_account( account, setabi.account) ) {
            create_account( setabi.account, now );
         }

         if( find_account( account, setabi.account) ) {
            fc::mutable_variant_object doc;
            abi_def abi_def = fc::raw::unpack<chain::abi_def>( setabi.abi );

            doc["name"] = account["name"];
            doc["abi"] = abi_def;
            doc["updateAt"] = now.count();
            doc["createAt"] = account["createAt"];

            auto json = fc::json::to_string( doc );
            try {
               elastic_client->index(accounts_type, json, account["_id"].as_string());
            } catch( ... ) {
               handle_elasticsearch_exception( "update account", __LINE__ );
            }
         }
      }
   } catch( fc::exception& e ) {
      // if unable to unpack native type, skip account creation
   }
}

bool elasticsearch_plugin_impl::add_action_trace( elasticlient::SameIndexBulkData& bulk_action_traces, const chain::action_trace& atrace,
                                        bool executed, const std::chrono::milliseconds& now )
{
   if( executed && atrace.receipt.receiver == chain::config::system_account_name ) {
      update_account( atrace.act );
   }

   bool added = false;
   if( start_block_reached && store_action_traces && filter_include( atrace.act ) ) {
      fc::mutable_variant_object action_traces_doc;
      const chain::base_action_trace& base = atrace; // without inline action traces

      fc::from_variant( to_variant_with_abi( base ), action_traces_doc );
      action_traces_doc["createdAt"] = now.count();

      auto json = fc::json::to_string(action_traces_doc);
      bulk_action_traces.indexDocument(action_traces_type, "", json);
      added = true;
   }

   for( const auto& iline_atrace : atrace.inline_traces ) {
      added |= add_action_trace( bulk_action_traces, iline_atrace, executed, now );
   }

   return added;
}

void elasticsearch_plugin_impl::_process_accepted_block( const chain::block_state_ptr& bs ) {

   auto block_num = bs->block_num;
   if( block_num % 1000 == 0 )
      ilog( "block_num: ${b}", ("b", block_num) );
   const auto block_id = bs->id;
   const auto block_id_str = block_id.str();
   const auto prev_block_id_str = bs->block->previous.str();

   auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()});

   fc::mutable_variant_object block_state_doc;
   block_state_doc["block_num"] = static_cast<int32_t>(block_num);
   block_state_doc["block_id"] = block_id_str;
   block_state_doc["validated"] = bs->validated;
   block_state_doc["in_current_chain"] = bs->in_current_chain;
   block_state_doc["block_header_state"] = bs;
   block_state_doc["createAt"] = now.count();

   auto block_states_json = fc::json::to_string( block_state_doc );

   try {
      elastic_client->index(block_states_type, block_states_json);
   } catch( ... ) {
      handle_elasticsearch_exception( "block_states index:" + block_states_json, __LINE__ );
   }

   if( !store_blocks ) return;

   fc::mutable_variant_object block_doc;

   block_doc["block_num"] = static_cast<int32_t>(block_num);
   block_doc["block_id"] = block_id_str;
   block_doc["irreversible"] = false;

   block_doc["block"] = to_variant_with_abi( *bs->block );
   block_doc["createAt"] = now.count();

   auto block_json = fc::json::to_string( block_doc );

   try {
      elastic_client->index(blocks_type, block_json);
   } catch( ... ) {
      handle_elasticsearch_exception( "block_states index:" + block_json, __LINE__ );
   }

}

void elasticsearch_plugin_impl::_process_accepted_transaction( const chain::transaction_metadata_ptr& t ) {
   fc::mutable_variant_object trans_doc;

   auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()} );

   const auto& trx_id = t->id;
   const auto trx_id_str = trx_id.str();
   const auto& trx = t->trx;

   fc::from_variant( to_variant_with_abi( trx ), trans_doc );
   trans_doc["trx_id"] = trx_id_str;

   fc::variant signing_keys;
   if( t->signing_keys.valid() ) {
      signing_keys = t->signing_keys->second;
   } else {
      signing_keys = trx.get_signature_keys( *chain_id, false, false );
   }

   if( !signing_keys.is_null() ) {
      trans_doc["signing_keys"] = signing_keys;
   }

   trans_doc["accepted"] = t->accepted;
   trans_doc["implicit"] = t->implicit;
   trans_doc["scheduled"] = t->scheduled;
   trans_doc["createdAt"] = now.count();

   auto trans_json = fc::json::to_string( trans_doc );

   try {
      elastic_client->index(trans_type, trans_json);
   } catch( ... ) {
      handle_elasticsearch_exception( "trans index:" + trans_json, __LINE__ );
   }
}

void elasticsearch_plugin_impl::_process_applied_transaction( const chain::transaction_trace_ptr& t ) {
   auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()});

   elasticlient::SameIndexBulkData bulk_action_traces(index_name);

   fc::mutable_variant_object trans_traces_doc;
   bool write_atraces = false;
   bool executed = t->receipt.valid() && t->receipt->status == chain::transaction_receipt_header::executed;

   for( const auto& atrace : t->action_traces ) {
      try {
         write_atraces |= add_action_trace( bulk_action_traces, atrace, executed, now );
      } catch(...) {
         handle_elasticsearch_exception("add action traces", __LINE__);
      }
   }

   if( write_atraces ) {
      try {
         elastic_client->bulk_perform(bulk_action_traces);
      } catch( ... ) {
         handle_elasticsearch_exception( "action traces:" + bulk_action_traces.body(), __LINE__ );
      }
   }

   if( !start_block_reached || !store_transaction_traces ) return;
   if( !write_atraces ) return; //< do not index transaction_trace if all action_traces filtered out

   // transaction trace index
   fc::from_variant( to_variant_with_abi( *t ), trans_traces_doc );
   trans_traces_doc["createAt"] = now.count();

   std::string json = fc::json::to_string( trans_traces_doc );
   try {
      elastic_client->index(trans_traces_type, json);
   } catch( ... ) {
      handle_elasticsearch_exception( "trans_traces index: " + json, __LINE__ );
   }

}

void elasticsearch_plugin_impl::consume_blocks() {
   try {
      while (true) {
         boost::mutex::scoped_lock lock(mtx);
         while ( transaction_metadata_queue.empty() &&
                 transaction_trace_queue.empty() &&
                 block_state_queue.empty() &&
                 irreversible_block_state_queue.empty() &&
                 !done ) {
            condition.wait(lock);
         }

         // capture for processing
         size_t transaction_metadata_size = transaction_metadata_queue.size();
         if (transaction_metadata_size > 0) {
            transaction_metadata_process_queue = move(transaction_metadata_queue);
            transaction_metadata_queue.clear();
         }
         size_t transaction_trace_size = transaction_trace_queue.size();
         if (transaction_trace_size > 0) {
            transaction_trace_process_queue = move(transaction_trace_queue);
            transaction_trace_queue.clear();
         }
         size_t block_state_size = block_state_queue.size();
         if (block_state_size > 0) {
            block_state_process_queue = move(block_state_queue);
            block_state_queue.clear();
         }
         size_t irreversible_block_size = irreversible_block_state_queue.size();
         if (irreversible_block_size > 0) {
            irreversible_block_state_process_queue = move(irreversible_block_state_queue);
            irreversible_block_state_queue.clear();
         }

         lock.unlock();

         if (done) {
            ilog("draining queue, size: ${q}", ("q", transaction_metadata_size + transaction_trace_size + block_state_size + irreversible_block_size));
         }

         // process transactions
         auto start_time = fc::time_point::now();
         auto size = transaction_trace_process_queue.size();
         while (!transaction_trace_process_queue.empty()) {
            const auto& t = transaction_trace_process_queue.front();
            process_applied_transaction(t);
            transaction_trace_process_queue.pop_front();
         }
         auto time = fc::time_point::now() - start_time;
         auto per = size > 0 ? time.count()/size : 0;
         if( time > fc::microseconds(500000) ) // reduce logging, .5 secs
            ilog( "process_applied_transaction,  time per: ${p}, size: ${s}, time: ${t}", ("s", size)("t", time)("p", per) );

         start_time = fc::time_point::now();
         size = transaction_metadata_process_queue.size();
         while (!transaction_metadata_process_queue.empty()) {
            const auto& t = transaction_metadata_process_queue.front();
            process_accepted_transaction(t);
            transaction_metadata_process_queue.pop_front();
         }
         time = fc::time_point::now() - start_time;
         per = size > 0 ? time.count()/size : 0;
         if( time > fc::microseconds(500000) ) // reduce logging, .5 secs
            ilog( "process_accepted_transaction, time per: ${p}, size: ${s}, time: ${t}", ("s", size)( "t", time )( "p", per ));

         // process blocks
         start_time = fc::time_point::now();
         size = block_state_process_queue.size();
         while (!block_state_process_queue.empty()) {
            const auto& bs = block_state_process_queue.front();
            process_accepted_block( bs );
            block_state_process_queue.pop_front();
         }
         time = fc::time_point::now() - start_time;
         per = size > 0 ? time.count()/size : 0;
         if( time > fc::microseconds(500000) ) // reduce logging, .5 secs
            ilog( "process_accepted_block,       time per: ${p}, size: ${s}, time: ${t}", ("s", size)("t", time)("p", per) );

         // process irreversible blocks
         start_time = fc::time_point::now();
         size = irreversible_block_state_process_queue.size();
         while (!irreversible_block_state_process_queue.empty()) {
            const auto& bs = irreversible_block_state_process_queue.front();
            process_irreversible_block(bs);
            irreversible_block_state_process_queue.pop_front();
         }
         time = fc::time_point::now() - start_time;
         per = size > 0 ? time.count()/size : 0;
         if( time > fc::microseconds(500000) ) // reduce logging, .5 secs
            ilog( "process_irreversible_block,   time per: ${p}, size: ${s}, time: ${t}", ("s", size)("t", time)("p", per) );

         if( transaction_metadata_size == 0 &&
             transaction_trace_size == 0 &&
             block_state_size == 0 &&
             irreversible_block_size == 0 &&
             done ) {
            break;
         }
      }
      ilog("elasticsearch_plugin consume thread shutdown gracefully");
   } catch (fc::exception& e) {
      elog("FC Exception while consuming block ${e}", ("e", e.to_string()));
   } catch (std::exception& e) {
      elog("STD Exception while consuming block ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while consuming block");
   }
}


void elasticsearch_plugin_impl::delete_index() {
   ilog("drop elasticsearch index");
   elastic_client->delete_index();
}

void elasticsearch_plugin_impl::init() {
   ilog("create elasticsearch index");
   elastic_client->init_index( elastic_mappings );

   if (elastic_client->count_doc(accounts_type) == 0) {
      auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()});
      create_account(name( chain::config::system_account_name ), now);
   }

   ilog("starting elasticsearch plugin thread");
   consume_thread = boost::thread([this] { consume_blocks(); });

   startup = false;
}

elasticsearch_plugin::elasticsearch_plugin():my(new elasticsearch_plugin_impl()){}
elasticsearch_plugin::~elasticsearch_plugin(){}

void elasticsearch_plugin::set_program_options(options_description&, options_description& cfg) {
   cfg.add_options()
         ("option-name", bpo::value<string>()->default_value("default value"),
          "Option Description")
         ;
}

void elasticsearch_plugin::plugin_initialize(const variables_map& options) {
   ilog( "initializing elasticsearch_plugin" );
   try {
      if( options.count( "option-name" )) {
         // Handle the option
      }

      my->max_queue_size = 1024;

      my->abi_cache_size = 2048;

      my->abi_serializer_max_time = app().get_plugin<chain_plugin>().get_abi_serializer_max_time();

      my->start_block_num = 0;


      if( my->start_block_num == 0 ) {
        my->start_block_reached = true;
      }

      my->delete_index_on_startup = true;

      my->index_name = "eos";
      my->elastic_client = std::make_shared<elasticsearch_client>(std::vector<std::string>({"http://localhost:9200/"}), "eos");

      // hook up to signals on controller
      chain_plugin* chain_plug = app().find_plugin<chain_plugin>();
      EOS_ASSERT( chain_plug, chain::missing_chain_plugin_exception, ""  );
      auto& chain = chain_plug->chain();
      my->chain_id.emplace( chain.get_chain_id());

      my->accepted_block_connection.emplace(
         chain.accepted_block.connect( [&]( const chain::block_state_ptr& bs ) {
         my->accepted_block( bs );
      } ));
      my->irreversible_block_connection.emplace(
         chain.irreversible_block.connect( [&]( const chain::block_state_ptr& bs ) {
            my->applied_irreversible_block( bs );
         } ));
      my->accepted_transaction_connection.emplace(
         chain.accepted_transaction.connect( [&]( const chain::transaction_metadata_ptr& t ) {
            my->accepted_transaction( t );
         } ));
      my->applied_transaction_connection.emplace(
         chain.applied_transaction.connect( [&]( const chain::transaction_trace_ptr& t ) {
            my->applied_transaction( t );
         } ));
      if( my->delete_index_on_startup ) {
         my->delete_index();
      }
      my->init();
   }
   FC_LOG_AND_RETHROW()
}

void elasticsearch_plugin::plugin_startup() {
   // Make the magic happen
}

void elasticsearch_plugin::plugin_shutdown() {
   my->accepted_block_connection.reset();
   my->irreversible_block_connection.reset();
   my->accepted_transaction_connection.reset();
   my->applied_transaction_connection.reset();

   my.reset();
}

}
