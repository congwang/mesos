/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __PROTOBUF_UTILS_HPP__
#define __PROTOBUF_UTILS_HPP__

#include <string>

#include <mesos/scheduler/scheduler.hpp>

#include <mesos/slave/isolator.hpp>

#include <stout/ip.hpp>
#include <stout/option.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp"

// Forward declaration (in lieu of an include).
namespace process {
struct UPID;
}

namespace mesos {
namespace internal {
namespace protobuf {

bool isTerminalState(const TaskState& state);


// See TaskStatus for more information about these fields. Note
// that the 'uuid' must be provided for updates that need
// acknowledgement. Currently, all slave and executor generated
// updates require acknowledgement, whereas master generated
// and scheduler driver generated updates do not.
StatusUpdate createStatusUpdate(
    const FrameworkID& frameworkId,
    const Option<SlaveID>& slaveId,
    const TaskID& taskId,
    const TaskState& state,
    const TaskStatus::Source& source,
    const Option<UUID>& uuid,
    const std::string& message = "",
    const Option<TaskStatus::Reason>& reason = None(),
    const Option<ExecutorID>& executorId = None(),
    const Option<bool>& healthy = None(),
    const Option<Labels>& labels = None());


Task createTask(
    const TaskInfo& task,
    const TaskState& state,
    const FrameworkID& frameworkId);


Option<bool> getTaskHealth(const Task& task);


// Helper function that creates a MasterInfo from UPID.
MasterInfo createMasterInfo(const process::UPID& pid);


Label createLabel(const std::string& key, const std::string& value);

namespace slave {

mesos::slave::ContainerLimitation createContainerLimitation(
    const Resources& resources,
    const std::string& message);


mesos::slave::ContainerState createContainerState(
    const ExecutorInfo& executorInfo,
    const ContainerID& id,
    pid_t pid,
    const std::string& directory,
    const Option<std::string>& rootfs);

} // namespace slave {

namespace scheduler {

// Helper functions that create scheduler::Event from a message that
// is sent to the scheduler.
mesos::scheduler::Event event(const FrameworkRegisteredMessage& message);
mesos::scheduler::Event event(const FrameworkReregisteredMessage& message);
mesos::scheduler::Event event(const ResourceOffersMessage& message);
mesos::scheduler::Event event(const RescindResourceOfferMessage& message);
mesos::scheduler::Event event(const StatusUpdateMessage& message);
mesos::scheduler::Event event(const LostSlaveMessage& message);
mesos::scheduler::Event event(const ExitedExecutorMessage& message);
mesos::scheduler::Event event(const ExecutorToFrameworkMessage& message);
mesos::scheduler::Event event(const FrameworkErrorMessage& message);

} // namespace scheduler {

} // namespace protobuf {
} // namespace internal {
} // namespace mesos {

#endif // __PROTOBUF_UTILS_HPP__
