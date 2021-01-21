// Copyright 2020 The ShaderTrap Project Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef LIBSHADERTRAP_COMMAND_BIND_STORAGE_BUFFER_H
#define LIBSHADERTRAP_COMMAND_BIND_STORAGE_BUFFER_H

#include <cstdint>
#include <memory>
#include <string>

#include "libshadertrap/command.h"
#include "libshadertrap/token.h"

namespace shadertrap {

class CommandBindStorageBuffer : public Command {
 public:
  CommandBindStorageBuffer(std::unique_ptr<Token> start_token,
                           std::string storage_buffer_identifier,
                           uint32_t binding);

  bool Accept(CommandVisitor* visitor) override;

  const std::string& GetStorageBufferIdentifier() const {
    return storage_buffer_identifier_;
  }

  uint32_t GetBinding() const { return binding_; }

 private:
  std::string storage_buffer_identifier_;
  uint32_t binding_;
};

}  // namespace shadertrap

#endif  // LIBSHADERTRAP_COMMAND_BIND_STORAGE_BUFFER_H
