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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_REST_HANDLER_REST_JOB_HANDLER_H
#define ARANGOD_REST_HANDLER_REST_JOB_HANDLER_H 1

#include "Basics/Common.h"
#include "GeneralServer/AsyncJobManager.h"
#include "RestHandler/RestBaseHandler.h"

namespace arangodb {
namespace rest {
class AsyncJobManager;
class Dispatcher;
}  // namespace rest

////////////////////////////////////////////////////////////////////////////////
/// @brief job control request handler
////////////////////////////////////////////////////////////////////////////////

class RestJobHandler : public RestBaseHandler {
 public:
  RestJobHandler(application_features::ApplicationServer&, GeneralRequest*,
                 GeneralResponse*, rest::AsyncJobManager*);

 public:
  char const* name() const override final { return "RestJobHandler"; }
  RequestLane lane() const override final { return RequestLane::CLIENT_FAST; }
  RestStatus execute() override;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief put handler
  //////////////////////////////////////////////////////////////////////////////

  void putJob();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief put method handler
  //////////////////////////////////////////////////////////////////////////////

  void putJobMethod();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get handler
  //////////////////////////////////////////////////////////////////////////////

  void getJob();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get a job's status by its id
  //////////////////////////////////////////////////////////////////////////////

  void getJobById(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get job status by type
  //////////////////////////////////////////////////////////////////////////////

  void getJobByType(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief delete handler
  //////////////////////////////////////////////////////////////////////////////

  void deleteJob();

 protected:
  virtual std::string forwardingTarget() override;

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief async job manager
  //////////////////////////////////////////////////////////////////////////////

  rest::AsyncJobManager* _jobManager;
};
}  // namespace arangodb

#endif
