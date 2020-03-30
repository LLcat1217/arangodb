////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2017 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "TailingSyncer.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Basics/NumberUtils.h"
#include "Basics/ReadLocker.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringBuffer.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/system-functions.h"
#include "Logger/Logger.h"
#include "Replication/InitialSyncer.h"
#include "Replication/ReplicationApplier.h"
#include "Replication/ReplicationTransaction.h"
#include "Rest/HttpRequest.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "SimpleHttpClient/SimpleHttpClient.h"
#include "SimpleHttpClient/SimpleHttpResult.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Hints.h"
#include "Utils/CollectionGuard.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/Methods/Databases.h"
#include "VocBase/voc-types.h"
#include "VocBase/vocbase.h"

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Parser.h>
#include <velocypack/Slice.h>
#include <velocypack/StringRef.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::httpclient;
using namespace arangodb::rest;

namespace {

static arangodb::velocypack::StringRef const cnameRef("cname");
static arangodb::velocypack::StringRef const dataRef("data");
static arangodb::velocypack::StringRef const tickRef("tick");
static arangodb::velocypack::StringRef const dbRef("db");

bool hasHeader(std::unique_ptr<httpclient::SimpleHttpResult> const& response,
               std::string const& name) {
  return response->hasHeaderField(name);
}

bool getBoolHeader(std::unique_ptr<httpclient::SimpleHttpResult> const& response,
                   std::string const& name) {
  bool found = false;
  std::string header = response->getHeaderField(name, found);
  if (found) {
    return StringUtils::boolean(header);
  }
  return false;
}

uint64_t getUIntHeader(std::unique_ptr<httpclient::SimpleHttpResult> const& response,
                       std::string const& name) {
  bool found = false;
  std::string header = response->getHeaderField(name, found);
  if (found) {
    return StringUtils::uint64(header);
  }
  return 0;
}

}  // namespace

/// @brief base url of the replication API
std::string const TailingSyncer::WalAccessUrl = "/_api/wal";

TailingSyncer::TailingSyncer(ReplicationApplier* applier,
                             ReplicationApplierConfiguration const& configuration,
                             TRI_voc_tick_t initialTick, bool useTick, TRI_voc_tick_t barrierId)
    : Syncer(configuration),
      _applier(applier),
      _hasWrittenState(false),
      _initialTick(initialTick),
      _usersModified(false),
      _useTick(useTick),
      _requireFromPresent(configuration._requireFromPresent),
      _ignoreRenameCreateDrop(false),
      _ignoreDatabaseMarkers(true),
      _workInParallel(false) {
  if (barrierId > 0) {
    _state.barrier.id = barrierId;
    _state.barrier.updateTime = TRI_microtime();
  }

  // FIXME: move this into engine code
  std::string const& engineName = EngineSelectorFeature::ENGINE->typeName();

  // Replication for RocksDB expects only one open transaction at a time
  _supportsMultipleOpenTransactions = (engineName != "rocksdb");
}

TailingSyncer::~TailingSyncer() { abortOngoingTransactions(); }

/// @brief decide based on _state.master which api to use
///        GlobalTailingSyncer should overwrite this probably
std::string TailingSyncer::tailingBaseUrl(std::string const& cc) {
  bool act32 = _state.master.simulate32Client();
  std::string const& base = act32 ? replutils::ReplicationUrl : TailingSyncer::WalAccessUrl;
  if (act32) {  // fallback pre 3.3
    if (cc == "tail") {
      return base + "/logger-follow?";
    } else if (cc == "open-transactions") {
      return base + "/determine-open-transactions?";
    }
    // should not be used for anything else
    TRI_ASSERT(false);
  }
  return base + "/" + cc + "?";
}

/// @brief set the applier progress
void TailingSyncer::setProgress(std::string const& msg) {
  if (_state.applier._verbose) {
    LOG_TOPIC("cb1ba", INFO, Logger::REPLICATION) << msg;
  } else {
    LOG_TOPIC("452fc", DEBUG, Logger::REPLICATION) << msg;
  }
  _applier->setProgress(msg);
}

/// @brief abort all ongoing transactions
void TailingSyncer::abortOngoingTransactions() noexcept {
  try {
    // abort all running transactions
    _ongoingTransactions.clear();
  } catch (...) {
    // ignore errors here
  }
}

/// @brief abort all ongoing transactions for a specific database 
void TailingSyncer::abortOngoingTransactions(std::string const& dbName) {
  for (auto it = _ongoingTransactions.begin(); it != _ongoingTransactions.end(); /* no hoisting */) {
    auto& trx = (*it).second;
    if (trx != nullptr && trx->vocbase().name() == dbName) {
      LOG_TOPIC("c1ec8", TRACE, Logger::REPLICATION) << "aborting open transaction for db " << dbName;
      it = _ongoingTransactions.erase(it);
    } else {
      ++it;
    }
  }
}

/// @brief count all ongoing transactions for a specific database 
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
size_t TailingSyncer::countOngoingTransactions(VPackSlice slice) const {
  size_t result = 0;

  TRI_ASSERT(slice.isObject());
  VPackSlice nameSlice = slice.get(::dbRef);
  
  if (nameSlice.isString()) {
    for (auto const& it : _ongoingTransactions) {
      auto const& trx = it.second;
      if (trx != nullptr && arangodb::velocypack::StringRef(nameSlice) == trx->vocbase().name()) {
        ++result;
      }
    }
  }

  return result;
}
#endif

/// @brief whether or not the are multiple ongoing transactions for one
/// database
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
bool TailingSyncer::hasMultipleOngoingTransactions() const {
  std::unordered_set<TRI_voc_tick_t> found;
  for (auto const& it : _ongoingTransactions) {
    auto const& trx = it.second;
    if (trx != nullptr && !found.emplace(trx->vocbase().id()).second) {
      // found a duplicate
      return true;
    }
  }
  return false;
}
#endif

/// @brief whether or not a marker should be skipped
bool TailingSyncer::skipMarker(TRI_voc_tick_t firstRegularTick, VPackSlice const& slice,
                               TRI_voc_tick_t actualMarkerTick, TRI_replication_operation_e type) {
  TRI_ASSERT(slice.isObject());

  bool tooOld = (actualMarkerTick < firstRegularTick);

  if (tooOld) {
    // handle marker type

    if (type == REPLICATION_MARKER_DOCUMENT || type == REPLICATION_MARKER_REMOVE ||
        type == REPLICATION_TRANSACTION_START || type == REPLICATION_TRANSACTION_ABORT ||
        type == REPLICATION_TRANSACTION_COMMIT) {
      // read "tid" entry from marker
      VPackSlice tidSlice = slice.get("tid");
      if (tidSlice.isString() && tidSlice.getStringLength() > 0) {
        VPackValueLength len;
        char const* str = tidSlice.getStringUnchecked(len);
        TRI_voc_tid_t tid = NumberUtils::atoi_zero<TRI_voc_tid_t>(str, str + len);

        if (tid > 0 && _ongoingTransactions.find(tid) != _ongoingTransactions.end()) {
          // must still use this marker as it belongs to a transaction we need
          // to finish
          tooOld = false;
        }
      }
    }
  }

  if (tooOld) {
    return true;
  }

  // the transient applier state is just used for one shard / collection
  if (_state.applier._restrictCollections.empty()) {
    return false;
  }

  if (_state.applier._restrictType == ReplicationApplierConfiguration::RestrictType::None &&
      _state.applier._includeSystem) {
    return false;
  }

  VPackSlice const name = slice.get(::cnameRef);
  if (name.isString()) {
    return isExcludedCollection(name.copyString());
  }

  // call virtual method
  return skipMarker(slice);
}

/// @brief whether or not a collection should be excluded
bool TailingSyncer::isExcludedCollection(std::string const& masterName) const {
  if (masterName[0] == '_' && !_state.applier._includeSystem) {
    // system collection
    return true;
  }

  auto const it = _state.applier._restrictCollections.find(masterName);

  bool found = (it != _state.applier._restrictCollections.end());

  if (_state.applier._restrictType == ReplicationApplierConfiguration::RestrictType::Include &&
      !found) {
    // collection should not be included
    return true;
  } else if (_state.applier._restrictType == ReplicationApplierConfiguration::RestrictType::Exclude &&
             found) {
    // collection should be excluded
    return true;
  }

  if (TRI_ExcludeCollectionReplication(masterName, /*includeSystem*/true,
                                       _state.applier._includeFoxxQueues)) {
    return true;
  }

  return false;
}

/// @brief process db create or drop markers
Result TailingSyncer::processDBMarker(TRI_replication_operation_e type,
                                      velocypack::Slice const& slice) {
  TRI_ASSERT(!_ignoreDatabaseMarkers);

  // the new wal access protocol contains database names
  VPackSlice const nameSlice = slice.get(::dbRef);
  if (!nameSlice.isString()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_REPLICATION_INVALID_RESPONSE,
        "create/drop database marker did not contain name");
  }
  std::string name = nameSlice.copyString();
  if (name.empty() || (name[0] >= '0' && name[0] <= '9')) {
    LOG_TOPIC("e9bdc", ERR, Logger::REPLICATION) << "invalid database name in log";
    return Result(TRI_ERROR_ARANGO_DATABASE_NAME_INVALID);
  }

  if (!_state.applier._server.hasFeature<arangodb::SystemDatabaseFeature>()) {
    return arangodb::Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }
  auto& sysDbFeature =
      _state.applier._server.getFeature<arangodb::SystemDatabaseFeature>();

  if (type == REPLICATION_DATABASE_CREATE) {
    VPackSlice const data = slice.get("data");

    if (!data.isObject()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_REPLICATION_INVALID_RESPONSE,
          "create database marker did not contain data");
    }
    TRI_ASSERT(basics::VelocyPackHelper::equal(data.get("name"), nameSlice, false));

    TRI_vocbase_t* vocbase = DatabaseFeature::DATABASE->lookupDatabase(name);

    if (vocbase != nullptr && name != StaticStrings::SystemDatabase) {
      LOG_TOPIC("0a3a4", WARN, Logger::REPLICATION)
          << "seeing database creation marker "
          << "for an already existing db. Dropping db...";

      auto system = sysDbFeature.use();
      TRI_ASSERT(system.get());
      auto res = methods::Databases::drop(system.get(), name);

      if (res.fail()) {
        LOG_TOPIC("e8595", ERR, Logger::REPLICATION) << res.errorMessage();
        return res;
      }
    }

    VPackSlice users = VPackSlice::emptyArraySlice();
    Result res = methods::Databases::create(_state.applier._server, name, users,
                                            VPackSlice::emptyObjectSlice());

    return res;
  } else if (type == REPLICATION_DATABASE_DROP) {
    TRI_vocbase_t* vocbase = DatabaseFeature::DATABASE->lookupDatabase(name);

    if (vocbase != nullptr && name != StaticStrings::SystemDatabase) {
      // abort all ongoing transactions for the database to be dropped
      abortOngoingTransactions(name);

      auto system = sysDbFeature.use();
      TRI_ASSERT(system.get());
      // delete from cache by id and name
      _state.vocbases.erase(std::to_string(vocbase->id()));
      _state.vocbases.erase(name);

      auto res = methods::Databases::drop(system.get(), name);

      if (res.fail()) {
        LOG_TOPIC("21b6a", ERR, Logger::REPLICATION) << res.errorMessage();
      }

      return res;
    }

    return TRI_ERROR_NO_ERROR;  // ignoring because it's idempotent
  }

  TRI_ASSERT(false);
  return Result(TRI_ERROR_INTERNAL);  // unreachable
}

/// @brief process a document operation, based on the VelocyPack provided
Result TailingSyncer::processDocument(TRI_replication_operation_e type,
                                      VPackSlice const& slice) {
  TRI_vocbase_t* vocbase = resolveVocbase(slice);

  if (vocbase == nullptr) {
    return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  auto* coll = resolveCollection(*vocbase, slice).get();

  if (coll == nullptr) {
    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  bool const isSystem = coll->system();
  bool const isUsers = coll->name() == TRI_COL_NAME_USERS; 

  // extract "data"
  VPackSlice const data = slice.get(::dataRef);

  if (!data.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "invalid document format");
  }

  // extract "key"
  VPackSlice const key = data.get(StaticStrings::KeyString);

  if (!key.isString()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "invalid document key format");
  }

  // extract "rev"
  VPackSlice const rev = data.get(StaticStrings::RevString);

  if (!rev.isNone() && !rev.isString()) {
    // _rev is an optional attribute
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "invalid document revision format");
  }

  // extract "tid"
  arangodb::velocypack::StringRef const transactionId =
      VelocyPackHelper::getStringRef(slice, "tid", VPackStringRef());
  TRI_voc_tid_t tid = 0;
  if (!transactionId.empty()) {
    // operation is part of a transaction
    tid = NumberUtils::atoi_zero<TRI_voc_tid_t>(transactionId.data(),
                                                transactionId.data() +
                                                    transactionId.size());
  }

  // in case this is a removal we need to build our marker
  VPackSlice applySlice = data;
  if (type == REPLICATION_MARKER_REMOVE) {
    _documentBuilder.clear();
    _documentBuilder.openObject(true);
    _documentBuilder.add(StaticStrings::KeyString, key);
    if (rev.isString()) {
      // _rev is an optional attribute
      _documentBuilder.add(StaticStrings::RevString, rev);
    }
    _documentBuilder.close();
    applySlice = _documentBuilder.slice();
  }

  if (tid > 0) {  // part of a transaction
    auto it = _ongoingTransactions.find(tid);

    if (it == _ongoingTransactions.end()) {
      return Result(TRI_ERROR_REPLICATION_UNEXPECTED_TRANSACTION,
                    std::string("unexpected transaction ") + StringUtils::itoa(tid));
    }

    std::unique_ptr<ReplicationTransaction>& trx = (*it).second;

    if (trx == nullptr) {
      return Result(TRI_ERROR_REPLICATION_UNEXPECTED_TRANSACTION,
                    std::string("unexpected transaction ") + StringUtils::itoa(tid));
    }

    trx->addCollectionAtRuntime(coll->id(), coll->name(), AccessMode::Type::EXCLUSIVE);
    Result r = applyCollectionDumpMarker(*trx, coll, type, applySlice);
    TRI_ASSERT(!r.is(TRI_ERROR_ARANGO_TRY_AGAIN));

    if (r.errorNumber() == TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED && isSystem) {
      // ignore unique constraint violations for system collections
      r.reset();
    }
    if (r.ok() && isUsers) {
      _usersModified = true;
    }

    return r;  // done
  }
    
  // standalone operation

  // this variable will store the key of a conflicting document we will have to remove first
  // it is initially empty, and may be populated by a failing operation
  std::string conflictDocumentKey;
    
  // normally we will go into this while loop just once. only in the very exceptional case
  // that there is a unique constraint violation in one of the secondary indexes we will
  // get into the while loop a second time
  int tries = 0;
  while (tries++ < 10) {
    if (!conflictDocumentKey.empty()) {
      // a rather exceptional case in which we have to remove a conflicting document,
      // which is conflicting with the to-be-inserted document in one the unique
      // secondary indexes

      // intentionally ignore the return code here, as the operation will be followed
      // by yet another insert/replace
      removeSingleDocument(coll, conflictDocumentKey);
      conflictDocumentKey.clear();
    }
    
    // update the apply tick for all standalone operations
    SingleCollectionTransaction trx(transaction::StandaloneContext::Create(*vocbase),
                                    *coll, AccessMode::Type::EXCLUSIVE);

    // we will always check if the target document already exists and then either
    // carry out an insert or a replace.
    // so we will be carrying out either a read-then-insert or a read-then-replace
    // operation, which is a single write operation. and for MMFiles this is also
    // safe as we have the exclusive lock on the underlying collection anyway 
    trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

    Result res = trx.begin();

    // fix error handling here when function returns result
    if (!res.ok()) {
      return Result(res.errorNumber(),
                    std::string("unable to create replication transaction: ") +
                        res.errorMessage());
    }

    res = applyCollectionDumpMarker(trx, coll, type, applySlice);

    if (res.is(TRI_ERROR_ARANGO_TRY_AGAIN)) {
      // TRY_AGAIN we will only be getting when there is a conflicting document.
      // the key of the conflicting document can be found in the errorMessage
      // of the result :-|
      TRI_ASSERT(type != REPLICATION_MARKER_REMOVE);
      TRI_ASSERT(!res.errorMessage().empty());
      conflictDocumentKey = std::move(res).errorMessage();
      // restart the while loop above
      continue;
    }

    if (res.errorNumber() == TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED && isSystem) {
      // ignore unique constraint violations for system collections
      res.reset();
    }

    // fix error handling here when function returns result
    if (res.ok()) {
      res = trx.commit();

      if (res.ok() && isUsers) {
        _usersModified = true;
      }
    }

    return res;
  }

  return Result(TRI_ERROR_INTERNAL, "invalid state reached in processDocument");
}

Result TailingSyncer::removeSingleDocument(LogicalCollection* coll, std::string const& key) {
  SingleCollectionTransaction trx(transaction::StandaloneContext::Create(coll->vocbase()),
                                  *coll, AccessMode::Type::EXCLUSIVE);

  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  Result res = trx.begin();
  if (res.fail()) {
    return res;
  }

  OperationOptions options;
  options.silent = true;
  options.ignoreRevs = true;
  options.isRestore = true;
  options.waitForSync = false;
            
  VPackBuilder tmp;
  tmp.add(VPackValue(key));
  
  OperationResult opRes = trx.remove(coll->name(), tmp.slice(), options);
  if (opRes.fail()) {
    return opRes.result;
  }
  
  return trx.commit();
}

/// @brief starts a transaction, based on the VelocyPack provided
Result TailingSyncer::startTransaction(VPackSlice const& slice) {
  // {"type":2200,"tid":"230920705812199", "database": "123",
  // "collections":[{"cid":"230920700700391","operations":10}]}

  TRI_vocbase_t* vocbase = resolveVocbase(slice);

  if (vocbase == nullptr) {
    // for any other case, return "database not found" and abort the replication
    return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  std::string const id = VelocyPackHelper::getStringValue(slice, "tid", "");
  if (id.empty()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "transaction id value is missing in slice");
  }

  // transaction id
  // note: this is the remote transaction id!
  TRI_voc_tid_t tid =
      NumberUtils::atoi_zero<TRI_voc_tid_t>(id.data(), id.data() + id.size());

  auto it = _ongoingTransactions.find(tid);

  if (it != _ongoingTransactions.end()) {
    // found a previous version of the same transaction - should not happen...
    _ongoingTransactions.erase(it);
  }

  TRI_ASSERT(tid > 0);

  LOG_TOPIC("e39dc", TRACE, Logger::REPLICATION) << "starting replication transaction " << tid;

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(_supportsMultipleOpenTransactions || countOngoingTransactions(slice) == 0);
#endif

  auto trx = std::make_unique<ReplicationTransaction>(*vocbase);
  Result res = trx->begin();

  if (res.ok()) {
    _ongoingTransactions[tid] = std::move(trx);
  }

  return res;
}

/// @brief aborts a transaction, based on the VelocyPack provided
Result TailingSyncer::abortTransaction(VPackSlice const& slice) {
  // {"type":2201,"tid":"230920705812199","collections":[{"cid":"230920700700391","operations":10}]}
  std::string const id = VelocyPackHelper::getStringValue(slice, "tid", "");

  if (id.empty()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "transaction id is missing in slice");
  }

  // transaction id
  // note: this is the remote transaction id!
  TRI_voc_tid_t const tid =
      NumberUtils::atoi_zero<TRI_voc_tid_t>(id.data(), id.data() + id.size());

  auto it = _ongoingTransactions.find(tid);

  if (it == _ongoingTransactions.end() || (*it).second == nullptr) {
    // invalid state, no transaction was started.
    return Result(TRI_ERROR_REPLICATION_UNEXPECTED_TRANSACTION);
  }

  TRI_ASSERT(tid > 0);

  LOG_TOPIC("19551", TRACE, Logger::REPLICATION) << "aborting replication transaction " << tid;

  _ongoingTransactions.erase(it);
  return Result();
}

/// @brief commits a transaction, based on the VelocyPack provided
Result TailingSyncer::commitTransaction(VPackSlice const& slice) {
  // {"type":2201,"tid":"230920705812199","collections":[{"cid":"230920700700391","operations":10}]}
  std::string const id = VelocyPackHelper::getStringValue(slice, "tid", "");

  if (id.empty()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "transaction id is missing in slice");
  }

  // transaction id
  // note: this is the remote transaction id!
  TRI_voc_tid_t const tid =
      NumberUtils::atoi_zero<TRI_voc_tid_t>(id.data(), id.data() + id.size());

  auto it = _ongoingTransactions.find(tid);

  if (it == _ongoingTransactions.end() || (*it).second == nullptr) {
    return Result(TRI_ERROR_REPLICATION_UNEXPECTED_TRANSACTION);
  }

  TRI_ASSERT(tid > 0);

  LOG_TOPIC("fb331", TRACE, Logger::REPLICATION) << "committing replication transaction " << tid;

  std::unique_ptr<ReplicationTransaction>& trx = (*it).second;
  Result res = trx->commit();

  _ongoingTransactions.erase(it);

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(_supportsMultipleOpenTransactions || countOngoingTransactions(slice) == 0);
#endif
  return res;
}

/// @brief renames a collection, based on the VelocyPack provided
Result TailingSyncer::renameCollection(VPackSlice const& slice) {
  if (!slice.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "rename slice is not an object");
  }

  VPackSlice collection = slice.get("collection");
  if (!collection.isObject()) {
    collection = slice.get("data");
  }

  if (!collection.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "collection slice is not an object");
  }

  std::string const name =
      VelocyPackHelper::getStringValue(collection, "name", "");

  if (name.empty()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "name attribute is missing in rename slice");
  }

  TRI_vocbase_t* vocbase = resolveVocbase(slice);

  if (vocbase == nullptr) {
    return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  arangodb::LogicalCollection* col = nullptr;

  if (slice.hasKey("cuid")) {
    col = resolveCollection(*vocbase, slice).get();

    if (col == nullptr) {
      return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, "unknown cuid");
    }
  } else if (collection.hasKey("oldName")) {
    col = vocbase->lookupCollection(collection.get("oldName").copyString()).get();

    if (col == nullptr) {
      return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                    "unknown old collection name");
    }
  } else {
    TRI_ASSERT(col == nullptr);
    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                  "unable to identify collection");
  }

  if (col->system()) {
    LOG_TOPIC("36d7e", WARN, Logger::REPLICATION) << "Renaming system collection " << col->name();
  }

  return vocbase->renameCollection(col->id(), name);
}

/// @brief changes the properties of a collection,
/// based on the VelocyPack provided
Result TailingSyncer::changeCollection(VPackSlice const& slice) {
  if (!slice.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "collection slice is no object");
  }

  VPackSlice data = slice.get("collection");
  if (!data.isObject()) {
    data = slice.get("data");
  }

  if (!data.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "data slice is no object");
  }

  VPackSlice d = data.get("deleted");
  bool const isDeleted = (d.isBool() && d.getBool());

  TRI_vocbase_t* vocbase = resolveVocbase(slice);

  if (vocbase == nullptr) {
    if (isDeleted) {
      // not a problem if a collection that is going to be deleted anyway
      // does not exist on slave
      return Result();
    }

    return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  auto col = resolveCollection(*vocbase, slice);

  if (!col) {
    if (isDeleted) {
      // not a problem if a collection that is going to be deleted anyway
      // does not exist on slave
      return Result();
    }

    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  arangodb::CollectionGuard guard(vocbase, col);

  return guard.collection()->properties(data, false);  // always a full-update
}

/// @brief truncate a collections. Assumes no trx are running
Result TailingSyncer::truncateCollection(arangodb::velocypack::Slice const& slice) {
  if (!slice.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "collection slice is no object");
  }

  TRI_vocbase_t* vocbase = resolveVocbase(slice);
  if (vocbase == nullptr) {
    return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }
  auto* col = resolveCollection(*vocbase, slice).get();
  if (col == nullptr) {
    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  if (!_ongoingTransactions.empty()) {
    const char* msg =
        "Encountered truncate but still have ongoing transactions";
    LOG_TOPIC("9e39e", ERR, Logger::REPLICATION) << msg;
    return Result(TRI_ERROR_REPLICATION_UNEXPECTED_TRANSACTION, msg);
  }

  SingleCollectionTransaction trx(transaction::StandaloneContext::Create(*vocbase),
                                  *col, AccessMode::Type::EXCLUSIVE);
  trx.addHint(transaction::Hints::Hint::INTERMEDIATE_COMMITS);
  trx.addHint(transaction::Hints::Hint::ALLOW_RANGE_DELETE);
  Result res = trx.begin();
  if (!res.ok()) {
    return res;
  }

  OperationOptions opts;
  opts.isRestore = true;
  OperationResult opRes = trx.truncate(col->name(), opts);

  if (opRes.fail()) {
    return opRes.result;
  }

  return trx.finish(opRes.result);
}

/// @brief changes the properties of a view,
/// based on the VelocyPack provided
Result TailingSyncer::changeView(VPackSlice const& slice) {
  if (!slice.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "view marker slice is no object");
  }

  VPackSlice data = slice.get("data");

  if (!data.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "data slice is no object in view change marker");
  }

  VPackSlice d = data.get("deleted");
  bool const isDeleted = (d.isBool() && d.getBool());

  TRI_vocbase_t* vocbase = resolveVocbase(slice);

  if (vocbase == nullptr) {
    if (isDeleted) {
      // not a problem if a view that is going to be deleted anyway
      // does not exist on slave
      return Result();
    }
    return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  VPackSlice guidSlice = data.get(StaticStrings::DataSourceGuid);

  if (!guidSlice.isString() || guidSlice.getStringLength() == 0) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "no guid specified for view");
  }

  auto view = vocbase->lookupView(guidSlice.copyString());

  if (view == nullptr) {
    if (isDeleted) {
      // not a problem if a collection that is going to be deleted anyway
      // does not exist on slave
      return Result();
    }

    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  VPackSlice nameSlice = data.get(StaticStrings::DataSourceName);

  if (nameSlice.isString() && !nameSlice.isEqualString(view->name())) {
    auto res = view->rename(nameSlice.copyString());

    if (!res.ok()) {
      return res;
    }
  }

  VPackSlice properties = data.get("properties");

  if (properties.isObject()) {
    return view->properties(properties, false);  // always a full-update
  }

  return {};
}

/// @brief apply a single marker from the continuous log
Result TailingSyncer::applyLogMarker(VPackSlice const& slice, 
                                     ApplyStats& applyStats,
                                     TRI_voc_tick_t firstRegularTick,
                                     TRI_voc_tick_t markerTick, 
                                     TRI_replication_operation_e type) {
  // handle marker type
  if (type == REPLICATION_MARKER_DOCUMENT || type == REPLICATION_MARKER_REMOVE) {
    if (type == REPLICATION_MARKER_DOCUMENT) {
      ++applyStats.processedDocuments;
    } else {
      ++applyStats.processedRemovals;
    }
    try {
      return processDocument(type, slice);
    } catch (basics::Exception const& ex) {
      return Result(ex.code(), ex.what());
    } catch (std::exception const& ex) {
      return Result(TRI_ERROR_INTERNAL, ex.what());
    } catch (...) {
      return Result(TRI_ERROR_INTERNAL, "unknown exception in processDocument");
    }
  }

  if (type == REPLICATION_TRANSACTION_START) {
    return startTransaction(slice);
  }

  if (type == REPLICATION_TRANSACTION_ABORT) {
    return abortTransaction(slice);
  }

  if (type == REPLICATION_TRANSACTION_COMMIT) {
    return commitTransaction(slice);
  }

  if (type == REPLICATION_COLLECTION_CREATE) {
    if (_ignoreRenameCreateDrop) {
      LOG_TOPIC("ff01b", DEBUG, Logger::REPLICATION) << "Ignoring collection marker";
      return Result();
    }

    TRI_vocbase_t* vocbase = resolveVocbase(slice);

    if (vocbase == nullptr) {
      LOG_TOPIC("627db", WARN, Logger::REPLICATION)
          << "Did not find database for " << slice.toJson();
      return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    }

    if (slice.get("collection").isObject()) {
      return createCollection(*vocbase, slice.get("collection"), nullptr);
    }

    return createCollection(*vocbase, slice.get("data"), nullptr);
  }

  if (type == REPLICATION_COLLECTION_DROP) {
    if (_ignoreRenameCreateDrop) {
      return TRI_ERROR_NO_ERROR;
    }

    return dropCollection(slice, false);
  }

  if (type == REPLICATION_COLLECTION_RENAME) {
    if (_ignoreRenameCreateDrop) {
      // do not execute rename operations
      return Result();
    }

    return renameCollection(slice);
  }

  if (type == REPLICATION_COLLECTION_CHANGE) {
    return changeCollection(slice);
  }

  if (type == REPLICATION_COLLECTION_TRUNCATE) {
    return truncateCollection(slice);
  }

  if (type == REPLICATION_INDEX_CREATE) {
    return createIndex(slice);
  }

  if (type == REPLICATION_INDEX_DROP) {
    return dropIndex(slice);
  }

  if (type == REPLICATION_VIEW_CREATE) {
    if (_ignoreRenameCreateDrop) {
      LOG_TOPIC("846a7", DEBUG, Logger::REPLICATION) << "Ignoring view create marker";
      return Result();
    }

    TRI_vocbase_t* vocbase = resolveVocbase(slice);

    if (vocbase == nullptr) {
      LOG_TOPIC("028ef", WARN, Logger::REPLICATION)
          << "Did not find database for " << slice.toJson();
      return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    }

    return createView(*vocbase, slice.get("data"));
  }

  if (type == REPLICATION_VIEW_DROP) {
    if (_ignoreRenameCreateDrop) {
      LOG_TOPIC("9ea21", DEBUG, Logger::REPLICATION) << "Ignoring view drop marker";
      return Result();
    }

    return dropView(slice, false);
  }

  if (type == REPLICATION_VIEW_CHANGE) {
    return changeView(slice);
  }

  if (type == REPLICATION_DATABASE_CREATE || type == REPLICATION_DATABASE_DROP) {
    if (_ignoreDatabaseMarkers) {
      LOG_TOPIC("4254e", DEBUG, Logger::REPLICATION) << "Ignoring database marker";
      return Result();
    }

    return processDBMarker(type, slice);
  }

  return Result(TRI_ERROR_REPLICATION_UNEXPECTED_MARKER,
                std::string("unexpected marker type ") + StringUtils::itoa(type));
}

/// @brief apply the data from the continuous log
Result TailingSyncer::applyLog(SimpleHttpResult* response, TRI_voc_tick_t firstRegularTick,
                               ApplyStats& applyStats, uint64_t& ignoreCount) {
  // reload users if they were modified
  _usersModified = false;
  auto reloader = [this]() {
    if (_usersModified) {
      // reload users after initial dump
      reloadUsers();
      _usersModified = false;
    }
  };
  TRI_DEFER(reloader());

  StringBuffer& data = response->getBody();
  char const* p = data.begin();
  char const* end = p + data.length();

  // buffer must end with a NUL byte
  TRI_ASSERT(*end == '\0');

  // TODO: re-use a builder!
  auto builder = std::make_shared<VPackBuilder>();

  while (p < end) {
    char const* q = static_cast<char const*>(memchr(p, '\n', (end - p)));

    if (q == nullptr) {
      q = end;
    }

    char const* lineStart = p;
    size_t const lineLength = q - p;

    if (lineLength < 2) {
      // we are done
      return Result();
    }

    TRI_ASSERT(q <= end);

    applyStats.processedMarkers++;

    builder->clear();
    try {
      VPackParser parser(builder);
      parser.parse(p, static_cast<size_t>(q - p));
    } catch (std::exception const& ex) {
      return Result(TRI_ERROR_HTTP_CORRUPTED_JSON, ex.what());
    } catch (...) {
      return Result(TRI_ERROR_OUT_OF_MEMORY);
    }

    p = q + 1;

    VPackSlice const slice = builder->slice();

    if (!slice.isObject()) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    "received invalid JSON data");
    }

    int typeValue = VelocyPackHelper::getNumericValue<int>(slice, "type", 0);
    TRI_replication_operation_e markerType = static_cast<TRI_replication_operation_e>(typeValue);
    TRI_voc_tick_t markerTick = 0;

    VPackSlice tickSlice = slice.get(::tickRef);

    if (tickSlice.isString() && tickSlice.getStringLength() > 0) {
      VPackValueLength len = 0;
      char const* str = tickSlice.getStringUnchecked(len);
      markerTick = NumberUtils::atoi_zero<TRI_voc_tick_t>(str, str + len);
    }

    // entry is skipped?
    bool skipped = skipMarker(firstRegularTick, slice, markerTick, markerType);

    if (!skipped) {
      Result res = applyLogMarker(slice, applyStats, firstRegularTick, markerTick, markerType);

      if (res.fail()) {
        // apply error
        std::string errorMsg = res.errorMessage();

        if (ignoreCount == 0) {
          if (lineLength > 1024) {
            errorMsg +=
                ", offending marker: " + std::string(lineStart, 1024) + "...";
          } else {
            errorMsg += ", offending marker: " + std::string(lineStart, lineLength);
          }

          res.reset(res.errorNumber(), errorMsg);
          return res;
        }

        ignoreCount--;
        LOG_TOPIC("c887a", WARN, Logger::REPLICATION)
            << "ignoring replication error for database '" << _state.databaseName
            << "': " << errorMsg;
      }
    }

    // update tick value
    WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);

    if (markerTick > firstRegularTick &&
        markerTick > _applier->_state._lastProcessedContinuousTick) {
      TRI_ASSERT(markerTick > 0);
      _applier->_state._lastProcessedContinuousTick = markerTick;
    }

    if (_applier->_state._lastProcessedContinuousTick > _applier->_state._lastAppliedContinuousTick) {
      _applier->_state._lastAppliedContinuousTick = _applier->_state._lastProcessedContinuousTick;
    }

    if (skipped) {
      ++_applier->_state._totalSkippedOperations;
    } else if (_ongoingTransactions.empty()) {
      _applier->_state._safeResumeTick = _applier->_state._lastProcessedContinuousTick;
    }
  }

  // reached the end
  return Result();
}

/// @brief run method, performs continuous synchronization
/// catches exceptions
Result TailingSyncer::run() {
  try {
    auto guard = scopeGuard([this]() {
      abortOngoingTransactions();
    });
    return runInternal();
  } catch (arangodb::basics::Exception const& ex) {
    return Result(ex.code(),
                  std::string("continuous synchronization for database '") +
                      _state.databaseName + "' failed with exception: " + ex.what());
  } catch (std::exception const& ex) {
    return Result(TRI_ERROR_INTERNAL,
                  std::string("continuous synchronization for database '") +
                      _state.databaseName + "' failed with exception: " + ex.what());
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL,
                  std::string("continuous synchronization for database '") +
                      _state.databaseName + "' failed with unknown exception");
  }
}

/// @brief run method, performs continuous synchronization
/// internal method, may throw exceptions
Result TailingSyncer::runInternal() {
  if (!_state.connection.valid()) {
    return Result(TRI_ERROR_INTERNAL);
  }

  setAborted(false);

  TRI_DEFER(
      if (!_state.isChildSyncer) { _state.barrier.remove(_state.connection); });
  uint64_t shortTermFailsInRow = 0;

retry:
  // just to be sure we are starting/restarting from a clean state
  abortOngoingTransactions();

  double const start = TRI_microtime();

  Result res;
  uint64_t connectRetries = 0;

  // reset failed connects
  {
    WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);
    _applier->_state._failedConnects = 0;
    _applier->_state.setStartTime(); 
  }

  while (true) {
    setProgress("fetching master state information");
    res = _state.master.getState(_state.connection, _state.isChildSyncer);

    if (res.is(TRI_ERROR_REPLICATION_NO_RESPONSE)) {
      // master error. try again after a sleep period
      connectRetries++;
      {
        WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);
        _applier->_state._failedConnects = connectRetries;
        _applier->_state._totalRequests++;
        _applier->_state._totalFailedConnects++;
      }

      if (connectRetries <= _state.applier._maxConnectRetries) {
        // check if we are aborted externally
        if (_applier->sleepIfStillActive(_state.applier._connectionRetryWaitTime)) {
          setProgress(
              "fetching master state information failed. will retry now. "
              "retries left: " +
              std::to_string(_state.applier._maxConnectRetries - connectRetries));
          continue;
        }

        // somebody stopped the applier
        res.reset(TRI_ERROR_REPLICATION_APPLIER_STOPPED);
      }
    }

    // we either got a connection or an error
    break;
  }

  if (res.ok()) {
    WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);
    try {
      getLocalState();
      _applier->_state._failedConnects = 0;
      _applier->_state._totalRequests++;
    } catch (basics::Exception const& ex) {
      res = Result(ex.code(), ex.what());
    } catch (std::exception const& ex) {
      res = Result(TRI_ERROR_INTERNAL, ex.what());
    } catch (...) {
      res.reset(TRI_ERROR_INTERNAL, "caught unknown exception");
    }
  }

  if (res.fail()) {
    // stop ourselves
    LOG_TOPIC("06384", INFO, Logger::REPLICATION) << "stopping applier: " << res.errorMessage();
    try {
      WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);
      _applier->_state._totalRequests++;
      getLocalState();
    } catch (basics::Exception const& ex) {
      res = Result(ex.code(), ex.what());
    } catch (std::exception const& ex) {
      res = Result(TRI_ERROR_INTERNAL, ex.what());
    } catch (...) {
      res.reset(TRI_ERROR_INTERNAL, "caught unknown exception");
    }

    _applier->stop(res);

    return res;
  }

  if (res.ok()) {
    res = runContinuousSync();
  }

  if (res.fail()) {
    // stop ourselves
    if (res.is(TRI_ERROR_REPLICATION_START_TICK_NOT_PRESENT) ||
        res.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND) || // data source -- collection or view
        res.is(TRI_ERROR_REPLICATION_NO_START_TICK)) {

      // additional logging
      if (res.is(TRI_ERROR_REPLICATION_START_TICK_NOT_PRESENT)) {
        LOG_TOPIC("a1040", WARN, Logger::REPLICATION)
            << "replication applier stopped for database '" << _applier->databaseName()
            << "' because required tick is not present on master";
      } else {
        LOG_TOPIC("33feb", WARN, Logger::REPLICATION)
            << "replication applier stopped for database '" << _applier->databaseName()
            << "': " << res.errorMessage();
      }

      // remove previous applier state
      abortOngoingTransactions(); //tries to clear map - no further side effects

      LOG_TOPIC("902e2", DEBUG, Logger::REPLICATION)
            << "stopped replication applier for database '" << _applier->databaseName();
      auto rv = _applier->resetState(true /*reducedSet*/);
      
      setAborted(false);

      if (rv.fail()) {
        return rv;
      }

      if (!_state.applier._autoResync) {
        LOG_TOPIC("713c2", INFO, Logger::REPLICATION)
            << "Auto resync disabled, applier for " << _applier->databaseName() << " will stop";
        _applier->stop(res);
        return res;
      }

      if (TRI_microtime() - start < 120.0) {
        // the applier only ran for less than 2 minutes. probably
        // auto-restarting it won't help much
        shortTermFailsInRow++;
      } else {
        shortTermFailsInRow = 0;
      }

      // check if we've made too many retries
      if (shortTermFailsInRow > _state.applier._autoResyncRetries) {
        if (_state.applier._autoResyncRetries > 0) {
          // message only makes sense if there's at least one retry
          LOG_TOPIC("91bb3", WARN, Logger::REPLICATION)
              << "aborting automatic resynchronization for " << _applier->databaseName()
              << " after " << _state.applier._autoResyncRetries << " short-term retries";
        } else {
          LOG_TOPIC("7db04", WARN, Logger::REPLICATION)
              << "aborting automatic resynchronization for " << _applier->databaseName()
              << " because autoResyncRetries is 0";
        }

        // always abort if we get here
        _applier->stop(res);
        return res;
      }
      
      // do an automatic full resync
      LOG_TOPIC("41845", WARN, Logger::REPLICATION)
          << "restarting initial synchronization for " << _applier->databaseName() 
          << " because autoResync option is set. retry #" << shortTermFailsInRow 
          << " of " << _state.applier._autoResyncRetries;

      {
        // increase number of syncs counter
        WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);
        ++_applier->_state._totalResyncs;

        // necessary to reset the state here, because otherwise running the
        // InitialSyncer may fail with "applier is running" errors
        _applier->_state._phase = ReplicationApplierState::ActivityPhase::INITIAL;
        _applier->_state._stopInitialSynchronization = false;
        _applier->_state._preventStart = false;
      }

      // start initial synchronization
      try {
        std::shared_ptr<InitialSyncer> syncer = _applier->buildInitialSyncer();
        Result r = syncer->run(_state.applier._incremental);
        if (r.ok()) {
          TRI_voc_tick_t lastLogTick = syncer->getLastLogTick();
          LOG_TOPIC("ee130", INFO, Logger::REPLICATION)
              << "automatic resynchronization for " << _applier->databaseName()
              << " finished. restarting continuous replication applier from "
                 "tick "
              << lastLogTick;
          _initialTick = lastLogTick;
          _useTick = true;
        
          {
            WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);
            _applier->_state.setStartTime();
          }
          _applier->markThreadTailing();
          goto retry;
        }
        res.reset(r.errorNumber(), r.errorMessage());
        LOG_TOPIC("45736", WARN, Logger::REPLICATION)
            << "initial replication for " << _applier->databaseName() << " failed: " << res.errorMessage();
        // fall through otherwise
      } catch (...) {
        res.reset(TRI_ERROR_INTERNAL,
                  "caught unknown exception during initial replication");
      }
        
      WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);
      _applier->_state._phase = ReplicationApplierState::ActivityPhase::INACTIVE;
    }
      
    abortOngoingTransactions();

    _applier->stop(res);
    return res;
  }

  return Result();
}

/// @brief get local replication apply state
void TailingSyncer::getLocalState() {
  uint64_t oldTotalRequests = _applier->_state._totalRequests;
  uint64_t oldTotalFailedConnects = _applier->_state._totalFailedConnects;

  bool const foundState = _applier->loadStateNoLock();
  _applier->_state._totalRequests = oldTotalRequests;
  _applier->_state._totalFailedConnects = oldTotalFailedConnects;

  if (!foundState) {
    // no state file found, so this is the initialization
    _applier->_state._serverId = _state.master.serverId;
    if (_useTick && _initialTick > 0) {
      _applier->_state._lastProcessedContinuousTick = _initialTick - 1;
      _applier->_state._lastAppliedContinuousTick = _initialTick - 1;
    }
    _applier->persistState(true);
    return;
  }

  // a _state.master.serverId value of 0 may occur if no proper connection could
  // be established to the master initially
  if (_state.master.serverId != _applier->_state._serverId &&
      _applier->_state._serverId.isSet() && _state.master.serverId.isSet()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_REPLICATION_MASTER_CHANGE,
        std::string(
            "encountered wrong master id in replication state file. found: ") +
            StringUtils::itoa(_state.master.serverId.id()) +
            ", expected: " + StringUtils::itoa(_applier->_state._serverId.id()));
  }
}

/// @brief perform a continuous sync with the master
Result TailingSyncer::runContinuousSync() {
  constexpr uint64_t MinWaitTime = 250 * 1000;        // 0.25 seconds
  constexpr uint64_t MaxWaitTime = 60 * 1000 * 1000;  // 60 seconds
  uint64_t connectRetries = 0;
  uint64_t inactiveCycles = 0;

  // get start tick
  // ---------------------------------------
  TRI_voc_tick_t fromTick = 0;
  TRI_voc_tick_t safeResumeTick = 0;

  {
    WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);

    if (_useTick) {
      // use user-defined tick
      fromTick = _initialTick;
      _applier->_state._lastAppliedContinuousTick = 0;
      _applier->_state._lastProcessedContinuousTick = 0;
      // saveApplierState();
    } else {
      // if we already transferred some data, we'll use the last applied tick
      LOG_TOPIC("7045d", DEBUG, Logger::REPLICATION)
          << "restarting continuous synchronization from previous state"
          << ", lastAppliedContinuousTick in state: " << _applier->_state._lastAppliedContinuousTick
          << ", lastProcessedContinuousTick in state: " << _applier->_state._lastProcessedContinuousTick
          << ", safeResumeTick in state: " << _applier->_state._safeResumeTick
          << ", fromTick: 0";
      
      fromTick = _applier->_state._lastAppliedContinuousTick;
      safeResumeTick = _applier->_state._safeResumeTick;
    }
  }

  LOG_TOPIC("daf0b", DEBUG, Logger::REPLICATION)
      << "requesting continuous synchronization, fromTick: " << fromTick
      << ", safeResumeTick " << safeResumeTick << ", useTick: " << _useTick
      << ", initialTick: " << _initialTick;

  if (fromTick == 0) {
    return Result(TRI_ERROR_REPLICATION_NO_START_TICK);
  }

  checkParallel();

  // get the applier into a sensible start state by fetching the list of
  // open transactions from the master
  TRI_voc_tick_t fetchTick = safeResumeTick;
  TRI_voc_tick_t lastScannedTick = safeResumeTick;  // hint where server MAY scan from
  if (safeResumeTick == 0 || safeResumeTick != fromTick) {
    // adjust fetchTick so we can tail starting from the tick containing
    // the open transactions we did not commit locally
    if (safeResumeTick > 0) {
      // important: we must not resume tailing in the middle of a RocksDB transaction,
      // as this would mean we would be missing the transaction begin marker. this would
      // cause "unexpected transaction errors"
      std::string const& engineName = EngineSelectorFeature::ENGINE->typeName();
      if (engineName == "rocksdb" && _state.master.engine == engineName) {
        fromTick = safeResumeTick;
      }
    }

    Result res = fetchOpenTransactions(safeResumeTick, fromTick, fetchTick);

    if (res.fail()) {
      return res;
    }
  }

  if (fetchTick > fromTick) {
    // must not happen
    return Result(TRI_ERROR_INTERNAL);
  }

  setProgress(std::string("starting with from tick ") + StringUtils::itoa(fromTick) +
              ", fetch tick " + StringUtils::itoa(fetchTick) +
              ", open transactions: " + StringUtils::itoa(_ongoingTransactions.size()) +
              ", parallel: " + (_workInParallel ? "yes" : "no"));

  // the shared status will wait in its destructor until all posted
  // requests have been completed/canceled!
  auto self = shared_from_this();
  auto sharedStatus = std::make_shared<Syncer::JobSynchronizer>(self);

  bool worked = false;
  bool mustFetchBatch = true;

  // run in a loop. the loop is terminated when the applier is stopped or an
  // error occurs
  while (true) {
    // fetchTick, worked and mustFetchBatch are passed by reference and are
    // updated by processMasterLog!

    // passing "mustFetchBatch = true" to processMasterLog will make it
    // initially fetch the next batch from the master passing "mustFetchBatch =
    // false" to processMasterLog requires that processMasterLog has already
    // requested the next batch in the background on the previous invocation
    TRI_ASSERT(mustFetchBatch || _workInParallel);

    Result res = processMasterLog(sharedStatus, fetchTick, lastScannedTick, fromTick,
                                  _state.applier._ignoreErrors, worked, mustFetchBatch);

    uint64_t sleepTime;

    if (res.is(TRI_ERROR_REPLICATION_NO_RESPONSE) ||
        res.is(TRI_ERROR_REPLICATION_MASTER_ERROR)) {
      // master error. try again after a sleep period
      if (_state.applier._connectionRetryWaitTime > 0) {
        sleepTime = _state.applier._connectionRetryWaitTime;
        if (sleepTime < MinWaitTime) {
          sleepTime = MinWaitTime;
        }
      } else {
        // default to prevent spinning too busy here
        sleepTime = 30 * 1000 * 1000;
      }

      connectRetries++;

      {
        WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);

        _applier->_state._failedConnects = connectRetries;
        _applier->_state._totalRequests++;
        _applier->_state._totalFailedConnects++;
      }

      if (connectRetries > _state.applier._maxConnectRetries) {
        // halt
        return res;
      }
    } else {
      connectRetries = 0;

      if (res.fail()) {
        // some other error we will not ignore
        return res;
      }

      // no error
      if (worked) {
        // we have done something, so we won't sleep (but check for cancelation)
        inactiveCycles = 0;
        sleepTime = 0;
      } else {
        sleepTime = _state.applier._idleMinWaitTime;
        if (sleepTime < MinWaitTime) {
          sleepTime = MinWaitTime;  // hard-coded minimum wait time
        }

        if (_state.applier._adaptivePolling) {
          inactiveCycles++;
          if (inactiveCycles > 60) {
            sleepTime *= 5;
          } else if (inactiveCycles > 30) {
            sleepTime *= 3;
          }
          if (inactiveCycles > 15) {
            sleepTime *= 2;
          }

          if (sleepTime > _state.applier._idleMaxWaitTime) {
            sleepTime = _state.applier._idleMaxWaitTime;
          }
        }

        if (sleepTime > MaxWaitTime) {
          sleepTime = MaxWaitTime;  // hard-coded maximum wait time
        }
      }
    }

    LOG_TOPIC("21d90", TRACE, Logger::REPLICATION)
        << "worked: " << worked << ", sleepTime: " << sleepTime;

    // this will make the applier thread sleep if there is nothing to do,
    // but will also check for cancelation
    if (!_applier->sleepIfStillActive(sleepTime)) {
      return Result(TRI_ERROR_REPLICATION_APPLIER_STOPPED);
    }
  }

  // won't be reached
  return Result(TRI_ERROR_INTERNAL);
}

/// @brief fetch the open transactions we still need to complete
Result TailingSyncer::fetchOpenTransactions(TRI_voc_tick_t fromTick, TRI_voc_tick_t toTick,
                                            TRI_voc_tick_t& startTick) {
  std::string const baseUrl = tailingBaseUrl("open-transactions");
  std::string const url = baseUrl + "serverId=" + _state.localServerIdString +
                          "&from=" + StringUtils::itoa(fromTick) +
                          "&to=" + StringUtils::itoa(toTick);

  {
    std::string const progress = "fetching initial master state with from tick " +
                                 StringUtils::itoa(fromTick) + ", to tick " +
                                 StringUtils::itoa(toTick);

    setProgress(progress);
  }

  // send request
  std::unique_ptr<httpclient::SimpleHttpResult> response;
  _state.connection.lease([&](httpclient::SimpleHttpClient* client) {
    response.reset(client->request(rest::RequestType::GET, url, nullptr, 0));
  });

  if (replutils::hasFailed(response.get())) {
    return replutils::buildHttpError(response.get(), url, _state.connection);
  }

  bool fromIncluded = false;

  bool found;
  std::string header =
      response->getHeaderField(StaticStrings::ReplicationHeaderFromPresent, found);

  if (found) {
    fromIncluded = StringUtils::boolean(header);
  }

  // fetch the tick from where we need to start scanning later
  header = response->getHeaderField(StaticStrings::ReplicationHeaderLastIncluded, found);
  if (!found) {
    // we changed the API in 3.3 to use last included
    header = response->getHeaderField(StaticStrings::ReplicationHeaderLastTick, found);
    if (!found) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    std::string("got invalid response from master at ") +
                        _state.master.endpoint + ": required header " +
                        StaticStrings::ReplicationHeaderLastTick +
                        " is missing in determine-open-transactions response");
    }
  }

  TRI_voc_tick_t readTick = StringUtils::uint64(header);

  if (!fromIncluded && fromTick > 0 &&
      (!_state.master.simulate32Client() || fromTick != readTick)) {
    Result r = handleRequiredFromPresentFailure(fromTick, readTick, "initial");
    TRI_ASSERT(_ongoingTransactions.empty());

    if (r.fail()) {
      return r;
    }
  }

  startTick = readTick;
  if (startTick == 0) {
    startTick = toTick;
  }

  VPackBuilder builder;
  Result r = replutils::parseResponse(builder, response.get());

  if (r.fail()) {
    return Result(
        TRI_ERROR_REPLICATION_INVALID_RESPONSE,
        std::string("got invalid response from master at ") + _state.master.endpoint +
            ": invalid response type for initial data. expecting array");
  }

  VPackSlice const slice = builder.slice();
  if (!slice.isArray()) {
    return Result(
        TRI_ERROR_REPLICATION_INVALID_RESPONSE,
        std::string("got invalid response from master at ") + _state.master.endpoint +
            ": invalid response type for initial data. expecting array");
  }

  for (VPackSlice it : VPackArrayIterator(slice)) {
    if (!it.isString()) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    std::string("got invalid response from master at ") +
                        _state.master.endpoint +
                        ": invalid response type for initial data. expecting "
                        "array of ids");
    }
    auto ref = it.stringRef();
    _ongoingTransactions.try_emplace(StringUtils::uint64(ref.data(), ref.size()), nullptr);
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(_supportsMultipleOpenTransactions || !hasMultipleOngoingTransactions());
#endif

  {
    std::string const progress =
        "fetched initial master state for from tick " +
        StringUtils::itoa(fromTick) + ", to tick " + StringUtils::itoa(toTick) +
        ", got start tick: " + StringUtils::itoa(readTick) +
        ", open transactions: " + std::to_string(_ongoingTransactions.size());

    setProgress(progress);
  }

  return Result();
}

/// @brief fetch data for the continuous synchronization
/// @param fetchTick tick from which we want results
/// @param lastScannedTick tick which the server MAY start scanning from
/// @param firstRegularTick if we got openTransactions server will return the
///                         only operations belonging to these for ticks <
///                         firstRegularTick
void TailingSyncer::fetchMasterLog(std::shared_ptr<Syncer::JobSynchronizer> sharedStatus,
                                   TRI_voc_tick_t fetchTick, TRI_voc_tick_t lastScannedTick,
                                   TRI_voc_tick_t firstRegularTick) {
  try {
    std::string const url =
        tailingBaseUrl("tail") +
        "chunkSize=" + StringUtils::itoa(_state.applier._chunkSize) +
        "&barrier=" + StringUtils::itoa(_state.barrier.id) +
        "&from=" + StringUtils::itoa(fetchTick) +
        "&lastScanned=" + StringUtils::itoa(lastScannedTick) +
        (firstRegularTick > fetchTick ? "&firstRegular=" + StringUtils::itoa(firstRegularTick)
                                      : "") +
        "&serverId=" + _state.localServerIdString +
        "&includeSystem=" + (_state.applier._includeSystem ? "true" : "false") +
        "&includeFoxxQueues=" + (_state.applier._includeFoxxQueues ? "true" : "false");
    
    // send request
    setProgress(std::string("fetching master log from tick ") + StringUtils::itoa(fetchTick) +
                ", last scanned tick " + StringUtils::itoa(lastScannedTick) +
                ", first regular tick " + StringUtils::itoa(firstRegularTick) +
                ", barrier: " + StringUtils::itoa(_state.barrier.id) +
                ", open transactions: " + std::to_string(_ongoingTransactions.size()) +
                ", chunk size " + std::to_string(_state.applier._chunkSize));

    // stringify list of open transactions
    VPackBuilder builder;
    builder.openArray();

    if (!_workInParallel) {
      // we must not access the list of ongoing transactions here
      // if we are in parallel mode. the reason is that another
      // thread may modify the list too, and this is not safe
      // note that we have made sure we do not need to send the list
      // of transactions when we are in parallel mode, as the parallel
      // mode is only supported for the RocksDB engine
      for (auto& it : _ongoingTransactions) {
        builder.add(VPackValue(StringUtils::itoa(it.first)));
      }
    }
    builder.close();

    std::string body = builder.slice().toJson();

    std::unique_ptr<httpclient::SimpleHttpResult> response;
    double time = TRI_microtime();

    _state.connection.lease([&](httpclient::SimpleHttpClient* client) {
      response.reset(client->request(rest::RequestType::PUT, url, body.c_str(),
                                     body.size()));
    });

    time = TRI_microtime() - time;

    if (replutils::hasFailed(response.get())) {
      // failure
      sharedStatus->gotResponse(
          replutils::buildHttpError(response.get(), url, _state.connection), time);
    } else {
      // success!
      LOG_TOPIC("a4822", DEBUG, Logger::REPLICATION) << "fetching master log from tick " + StringUtils::itoa(fetchTick) + " took " << time << " s";
      sharedStatus->gotResponse(std::move(response), time);
    }
  } catch (basics::Exception const& ex) {
    sharedStatus->gotResponse(Result(ex.code(), ex.what()));
  } catch (std::exception const& ex) {
    sharedStatus->gotResponse(Result(TRI_ERROR_INTERNAL, ex.what()));
  }
}

/// @brief apply continuous synchronization data from a batch
Result TailingSyncer::processMasterLog(std::shared_ptr<Syncer::JobSynchronizer> sharedStatus,
                                       TRI_voc_tick_t& fetchTick, TRI_voc_tick_t& lastScannedTick,
                                       TRI_voc_tick_t firstRegularTick, uint64_t& ignoreCount,
                                       bool& worked, bool& mustFetchBatch) {
  LOG_TOPIC("26a5b", DEBUG, Logger::REPLICATION)
      << "entering processMasterLog. fetchTick: " << fetchTick
      << ", worked: " << worked << ", mustFetchBatch: " << mustFetchBatch;

  // we either need to fetch a new batch here, or a batch must have been
  // request before (this is only possible in parallel mode)
  TRI_ASSERT(mustFetchBatch || _workInParallel);

  if (mustFetchBatch) {
    TRI_ASSERT(!sharedStatus->gotResponse());
    fetchMasterLog(sharedStatus, fetchTick, lastScannedTick, firstRegularTick);
  }

  // make sure that on the next invocation we will fetch a new batch
  // note that under some conditions we will fetch the next batch in the
  // background and will reset this value to false a bit more below
  mustFetchBatch = true;

  std::unique_ptr<httpclient::SimpleHttpResult> response;

  // block until we either got a response or were shut down
  Result res = sharedStatus->waitForResponse(response);

  if (res.fail()) {
    // no response or error or shutdown
    return res;
  }

  // now we have got a response!
  TRI_ASSERT(response != nullptr);

  worked = false;
  TRI_voc_tick_t const originalFetchTick = fetchTick;

  if (!hasHeader(response, StaticStrings::ReplicationHeaderCheckMore)) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  std::string("got invalid response from master at ") +
                      _state.master.endpoint + ": required header " +
                      StaticStrings::ReplicationHeaderCheckMore + " is missing");
  }

  bool checkMore = getBoolHeader(response, StaticStrings::ReplicationHeaderCheckMore);

  // was the specified from value included the result?
  bool const fromIncluded =
      getBoolHeader(response, StaticStrings::ReplicationHeaderFromPresent);
  lastScannedTick = getUIntHeader(response, StaticStrings::ReplicationHeaderLastScanned);

  if (!hasHeader(response, StaticStrings::ReplicationHeaderLastIncluded)) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  std::string("got invalid response from master at ") +
                      _state.master.endpoint + ": required header " +
                      StaticStrings::ReplicationHeaderLastIncluded +
                      " is missing in logger-follow response");
  }
    
  TRI_voc_tick_t lastIncludedTick =
      getUIntHeader(response, StaticStrings::ReplicationHeaderLastIncluded);
  TRI_voc_tick_t const tick = getUIntHeader(response, StaticStrings::ReplicationHeaderLastTick);
  
  LOG_TOPIC("5e543", DEBUG, Logger::REPLICATION) << "applyLog. fetchTick: " << fetchTick << ", checkMore: " << checkMore << ", fromIncluded: " << fromIncluded << ", lastScannedTick: " << lastScannedTick << ", lastIncludedTick: " << lastIncludedTick << ", tick: " << tick;
  
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // some temporary debug output here. TODO: remove this later when the assert does not trigger anymore
  if (tick < lastIncludedTick) {
    LOG_TOPIC("17ac5", ERR, Logger::FIXME) << "oops. tick: " << tick << ", lastIncludedTick: " << lastIncludedTick << ", response: " << response->getBody(); 
  }
#endif
  TRI_ASSERT(tick >= lastIncludedTick);

  if (lastIncludedTick == 0 && lastScannedTick > 0 && lastScannedTick > fetchTick) {
    // master did not have any news for us
    // still we can move forward the place from which to tail the WAL files
    fetchTick = lastScannedTick - 1;
  }

  if (lastIncludedTick > fetchTick) {
    fetchTick = lastIncludedTick;
    worked = true;
  } else {
    // we got the same tick again, this indicates we're at the end
    checkMore = false;
    LOG_TOPIC("425e4", DEBUG, Logger::REPLICATION) << "applyLog. got the same tick again, turning off checkMore";
  }

  if (!hasHeader(response, StaticStrings::ReplicationHeaderLastTick)) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  std::string("got invalid response from master at ") +
                      _state.master.endpoint + ": required header " +
                      StaticStrings::ReplicationHeaderLastTick +
                      " is missing in logger-follow response");
  }

  bool bumpTick = false;

  if (!checkMore && tick > lastIncludedTick) {
    // the master has a tick value which is not contained in this result
    // but it claims it does not have any more data
    // so it's probably a tick from an invisible operation (such as
    // closing a WAL file)
    bumpTick = true;
    LOG_TOPIC("854e2", DEBUG, Logger::REPLICATION) << "applyLog. bumping tick";
  }

  TRI_voc_tick_t lastAppliedTick;
  {
    WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);
    _applier->_state._lastAvailableContinuousTick = tick;

    lastAppliedTick = _applier->_state._lastAppliedContinuousTick;

    TRI_ASSERT(_applier->_state._lastAvailableContinuousTick >= _applier->_state._lastAppliedContinuousTick);

    _applier->_state._totalFetchTime += sharedStatus->time();
    _applier->_state._totalFetchInstances++;
  }

  if (!fromIncluded && fetchTick > 0 &&
      (!_state.master.simulate32Client() || originalFetchTick != tick)) {
    Result r = handleRequiredFromPresentFailure(fetchTick, tick, "ongoing");
    TRI_ASSERT(_ongoingTransactions.empty());

    if (r.fail()) {
      return r;
    }
  }

  // already fetch next batch of data in the background...
  if (_workInParallel && checkMore && !isAborted()) {
    TRI_ASSERT(worked);

    // do not fetch the same batch next time we enter processMasterLog
    // (that would be duplicate work)
    mustFetchBatch = false;
    auto self = shared_from_this();
    sharedStatus->request([this, self, sharedStatus, fetchTick, lastScannedTick,
                           firstRegularTick]() {
      fetchMasterLog(sharedStatus, fetchTick, lastScannedTick, firstRegularTick);
    });
  }

  ApplyStats applyStats;
  double time = TRI_microtime();
  Result r = applyLog(response.get(), firstRegularTick, applyStats, ignoreCount);
  time = TRI_microtime() - time;

  if (r.fail()) {
    LOG_TOPIC("04ba9", DEBUG, Logger::REPLICATION) << "applyLog failed with error: " << r.errorMessage();
    return r;
  }

  // success!
  LOG_TOPIC("608c2", DEBUG, Logger::REPLICATION) << "applyLog successful, lastAppliedTick: " << lastAppliedTick << ", firstRegularTick: " << firstRegularTick << ", processedMarkers: " << applyStats.processedMarkers << ", took: " << time << " s";

  // we grab the write-lock here and hold it until the end of this function
  WRITE_LOCKER_EVENTUAL(writeLocker, _applier->_statusLock);

  _applier->_state._failedConnects = 0;
  _applier->_state._totalRequests++;

  // cppcheck-suppress *
  if (applyStats.processedMarkers > 0) {
    worked = true;

    _applier->_state._totalApplyTime += time;
    _applier->_state._totalApplyInstances++;
    _applier->_state._totalEvents += applyStats.processedMarkers;
    _applier->_state._totalDocuments += applyStats.processedDocuments;
    _applier->_state._totalRemovals += applyStats.processedRemovals;

    if (_applier->_state._lastAppliedContinuousTick != lastAppliedTick) {
      _hasWrittenState = true;
      saveApplierState();
    }
    
    TRI_ASSERT(_applier->_state._lastAvailableContinuousTick >= _applier->_state._lastAppliedContinuousTick);
  } else if (bumpTick) {
    if (_applier->_state._lastProcessedContinuousTick < tick) {
      _applier->_state._lastProcessedContinuousTick = tick;
    }

    if (_ongoingTransactions.empty() && _applier->_state._safeResumeTick == 0) {
      _applier->_state._safeResumeTick = tick;
    }

    if (_ongoingTransactions.empty() && _applier->_state._lastAppliedContinuousTick == 0) {
      _applier->_state._lastAppliedContinuousTick = _applier->_state._lastProcessedContinuousTick;
    }

    if (!_hasWrittenState) {
      _hasWrittenState = true;
      saveApplierState();
    }
    
    TRI_ASSERT(_applier->_state._lastAvailableContinuousTick >= _applier->_state._lastAppliedContinuousTick);
  }

  if (!_hasWrittenState && _useTick) {
    // write state at least once so the start tick gets saved
    _hasWrittenState = true;

    _applier->_state._lastAppliedContinuousTick = firstRegularTick;
    _applier->_state._lastProcessedContinuousTick = firstRegularTick;

    if (_ongoingTransactions.empty() && _applier->_state._safeResumeTick == 0) {
      _applier->_state._safeResumeTick = firstRegularTick;
    }

    saveApplierState();
    
    TRI_ASSERT(_applier->_state._lastAvailableContinuousTick >= _applier->_state._lastAppliedContinuousTick);
  }

  if (!worked && checkMore) {
    worked = true;
  }

  return r;
}

void TailingSyncer::checkParallel() {
  // the default is to not work in parallel
  _workInParallel = false;

  if (_state.master.majorVersion < 3 ||
      (_state.master.majorVersion == 3 && _state.master.minorVersion < 4)) {
    // requires ArangoDB 3.4 or higher
    return;
  }

  std::string const& engineName = EngineSelectorFeature::ENGINE->typeName();
  if (engineName == "rocksdb" && _state.master.engine == engineName) {
    // master and slave are both on RocksDB... that means we do not need
    // to post the list of open transactions every time, and we can
    // also make the WAL tailing work in parallel on master and slave
    // in this case, the slave will post the next WAL tailing request
    // to the master in the background while it is applying the already
    // received WAL data from the master. this is only thread-safe if
    // we do not access the list of ongoing transactions in parallel
    _workInParallel = true;
  }
}

Result TailingSyncer::handleRequiredFromPresentFailure(TRI_voc_tick_t fromTick,
                                                       TRI_voc_tick_t readTick,
                                                       char const* type) {
  std::string const msg =
        std::string("required ") + type + " tick value '" + StringUtils::itoa(fromTick) +
        "' is not present (anymore?) on master at " + _state.master.endpoint +
        ". Last tick available on master is '" + StringUtils::itoa(readTick) +
        "'. It may be required to do a full resync and increase the number "
        "of historic logfiles/WAL file timeout or archive size on the master.";
  LOG_TOPIC("4c6d2", WARN, Logger::REPLICATION) << msg;

  if (_requireFromPresent) {  // hard fail
    abortOngoingTransactions();
    return Result(TRI_ERROR_REPLICATION_START_TICK_NOT_PRESENT, msg);
  }

  // only print a warning about the failure, abort ongoing transactions and go on...
  // we may have data loss and follow-up failures here, but at least all these
  // will be either logged or make the replication fail later on
    
  // we have to abort any running ongoing transactions, as they will be
  // holding exclusive locks on the underlying collection(s)
  if (!_ongoingTransactions.empty()) {
    LOG_TOPIC("63e32", WARN, Logger::REPLICATION) << "aborting ongoing open transactions (" << _ongoingTransactions.size() << ")";
    abortOngoingTransactions();
  }

  return Result();
}
