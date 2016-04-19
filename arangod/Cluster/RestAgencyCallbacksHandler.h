////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Andreas Streichardt
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_CLUSTER_REST_AGENCY_CALLBACKS_HANDLER_H
#define ARANGOD_CLUSTER_REST_AGENCY_CALLBACKS_HANDLER_H 1

#include "Basics/Common.h"
#include "RestHandler/RestVocbaseBaseHandler.h"

namespace arangodb {
class AgencyCallbackRegistry;

namespace rest {

////////////////////////////////////////////////////////////////////////////////
/// @brief shard control request handler
////////////////////////////////////////////////////////////////////////////////

class RestAgencyCallbacksHandler : public RestVocbaseBaseHandler {
 public:
  RestAgencyCallbacksHandler(HttpRequest* request, AgencyCallbackRegistry* agencyCallbackRegistry);

 public:
  bool isDirect() const override;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief executes the handler
  //////////////////////////////////////////////////////////////////////////////

  status_t execute() override;
 private:
  AgencyCallbackRegistry* _agencyCallbackRegistry;
};
}
}

#endif
