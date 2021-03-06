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

#include "libshadertrap/executor.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "libshadertrap/helpers.h"
#include "libshadertrap/uniform_value.h"
#include "libshadertrap/vertex_attribute_info.h"
#include "lodepng/lodepng.h"

// RGBA
#define CHANNELS (4)

namespace shadertrap {

Executor::Executor(MessageConsumer* message_consumer)
    : message_consumer_(message_consumer) {}

bool Executor::VisitAssertEqual(CommandAssertEqual* assert_equal) {
  if (created_renderbuffers_.count(assert_equal->GetBufferIdentifier1()) != 0) {
    return CheckEqualRenderbuffers(assert_equal);
  }
  assert(created_buffers_.count(assert_equal->GetBufferIdentifier1()) != 0);
  return CheckEqualBuffers(assert_equal);
}

bool Executor::VisitAssertPixels(CommandAssertPixels* assert_pixels) {
  GLuint framebuffer_object_id;
  GL_SAFECALL(glGenFramebuffers, 1, &framebuffer_object_id);
  GL_SAFECALL(glBindFramebuffer, GL_FRAMEBUFFER, framebuffer_object_id);
  GL_SAFECALL(
      glFramebufferRenderbuffer, GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_RENDERBUFFER,
      created_renderbuffers_.at(assert_pixels->GetRenderbufferIdentifier()));
  size_t width;
  size_t height;
  {
    GLint temp_width;
    GL_SAFECALL(glGetRenderbufferParameteriv, GL_RENDERBUFFER,
                GL_RENDERBUFFER_WIDTH, &temp_width);
    GLint temp_height;
    GL_SAFECALL(glGetRenderbufferParameteriv, GL_RENDERBUFFER,
                GL_RENDERBUFFER_HEIGHT, &temp_height);
    width = static_cast<size_t>(temp_width);
    height = static_cast<size_t>(temp_height);
  }

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    crash(
        "Problem with OpenGL framebuffer after specifying color render buffer: "
        "n%xn",
        status);
  }

  std::vector<std::uint8_t> data(width * height * CHANNELS);
  GL_SAFECALL(glReadBuffer, GL_COLOR_ATTACHMENT0);
  GL_SAFECALL(glReadPixels, 0, 0, static_cast<GLint>(width),
              static_cast<GLint>(height), GL_RGBA, GL_UNSIGNED_BYTE,
              data.data());
  for (size_t y = assert_pixels->GetRectangleY();
       y < assert_pixels->GetRectangleY() + assert_pixels->GetRectangleHeight();
       y++) {
    for (size_t x = assert_pixels->GetRectangleX();
         x <
         assert_pixels->GetRectangleX() + assert_pixels->GetRectangleWidth();
         x++) {
      uint8_t* start_of_pixel = &data[(height - y - 1) * width * 4 + x * 4];
      uint8_t r = start_of_pixel[0];
      uint8_t g = start_of_pixel[1];
      uint8_t b = start_of_pixel[2];
      uint8_t a = start_of_pixel[3];
      if (assert_pixels->GetExpectedR() != r ||
          assert_pixels->GetExpectedG() != g ||
          assert_pixels->GetExpectedB() != b ||
          assert_pixels->GetExpectedA() != a) {
        std::stringstream stringstream;
        stringstream << "Expected pixel ("
                     << static_cast<uint32_t>(assert_pixels->GetExpectedR())
                     << ", "
                     << static_cast<uint32_t>(assert_pixels->GetExpectedG())
                     << ", "
                     << static_cast<uint32_t>(assert_pixels->GetExpectedB())
                     << ", "
                     << static_cast<uint32_t>(assert_pixels->GetExpectedA())
                     << "), got (" << static_cast<uint32_t>(r) << ", "
                     << static_cast<uint32_t>(g) << ", "
                     << static_cast<uint32_t>(b) << ", "
                     << static_cast<uint32_t>(a) << ") at "
                     << assert_pixels->GetRenderbufferIdentifier() << "[" << x
                     << "][" << y << "]";
        message_consumer_->Message(MessageConsumer::Severity::kError,
                                   assert_pixels->GetStartToken(),
                                   stringstream.str());
      }
    }
  }
  return true;
}

bool Executor::VisitAssertSimilarEmdHistogram(
    CommandAssertSimilarEmdHistogram* assert_similar_emd_histogram) {
  GLuint renderbuffers[2];
  renderbuffers[0] = created_renderbuffers_.at(
      assert_similar_emd_histogram->GetBufferIdentifier1());
  renderbuffers[1] = created_renderbuffers_.at(
      assert_similar_emd_histogram->GetBufferIdentifier2());

  size_t width[2] = {0, 0};
  size_t height[2] = {0, 0};

  for (auto index : {0, 1}) {
    {
      GLint temp_width;
      GL_SAFECALL(glBindRenderbuffer, GL_RENDERBUFFER, renderbuffers[index]);
      GL_SAFECALL(glGetRenderbufferParameteriv, GL_RENDERBUFFER,
                  GL_RENDERBUFFER_WIDTH, &temp_width);
      width[index] = static_cast<size_t>(temp_width);
    }
    {
      GLint temp_height;
      GL_SAFECALL(glGetRenderbufferParameteriv, GL_RENDERBUFFER,
                  GL_RENDERBUFFER_HEIGHT, &temp_height);
      height[index] = static_cast<size_t>(temp_height);
    }
  }

  if (width[0] != width[1]) {
    std::stringstream stringstream;
    stringstream << "The widths of "
                 << assert_similar_emd_histogram->GetBufferIdentifier1()
                 << " and "
                 << assert_similar_emd_histogram->GetBufferIdentifier2()
                 << " do not match: " << width[0] << " vs. " << width[1];
    message_consumer_->Message(MessageConsumer::Severity::kError,
                               assert_similar_emd_histogram->GetStartToken(),
                               stringstream.str());
    return false;
  }

  if (height[0] != height[1]) {
    std::stringstream stringstream;
    stringstream << "The heights of "
                 << assert_similar_emd_histogram->GetBufferIdentifier1()
                 << " and "
                 << assert_similar_emd_histogram->GetBufferIdentifier2()
                 << " do not match: " << height[0] << " vs. " << height[1];
    message_consumer_->Message(MessageConsumer::Severity::kError,
                               assert_similar_emd_histogram->GetStartToken(),
                               stringstream.str());
    return false;
  }

  GLuint framebuffer_object_id;
  GL_SAFECALL(glGenFramebuffers, 1, &framebuffer_object_id);
  GL_SAFECALL(glBindFramebuffer, GL_FRAMEBUFFER, framebuffer_object_id);
  for (auto index : {0, 1}) {
    GL_SAFECALL(glFramebufferRenderbuffer, GL_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(index),
                GL_RENDERBUFFER, renderbuffers[index]);
  }
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    crash(
        "Problem with OpenGL framebuffer after specifying color render buffer: "
        "n%xn",
        status);
  }

  std::vector<std::uint8_t> data[2];
  for (auto index : {0, 1}) {
    data[index].resize(width[index] * height[index] * CHANNELS);
    GL_SAFECALL(glReadBuffer,
                GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(index));
    GL_SAFECALL(glReadPixels, 0, 0, static_cast<GLint>(width[index]),
                static_cast<GLint>(height[index]), GL_RGBA, GL_UNSIGNED_BYTE,
                data[index].data());
  }

  const size_t num_bins = 256;

  std::vector<std::vector<uint64_t>> histogram[2];
  for (auto index : {0, 1}) {
    for (size_t channel = 0; channel < 4; channel++) {
      histogram[index].emplace_back(std::vector<uint64_t>(num_bins, 0));
    }
    for (size_t y = 0; y < height[index]; y++) {
      for (size_t x = 0; x < width[index]; x++) {
        for (size_t channel = 0; channel < 4; channel++) {
          histogram[index][channel]
                   [data[index][y * width[index] * 4 + x * 4 + channel]]++;
        }
      }
    }
  }

  // Earth movers's distance: Calculate the minimal cost of moving "earth" to
  // transform the first histogram into the second, where each bin of the
  // histogram can be thought of as a column of units of earth. The cost is the
  // amount of earth moved times the distance carried (the distance is the
  // number of adjacent bins over which the earth is carried). Calculate this
  // using the cumulative difference of the bins, which works as long as both
  // histograms have the same amount of earth. Sum the absolute values of the
  // cumulative difference to get the final cost of how much (and how far) the
  // earth was moved.
  double max_emd = 0;

  for (size_t channel = 0; channel < 4; ++channel) {
    double diff_total = 0;
    double diff_accum = 0;

    for (size_t i = 0; i < num_bins; ++i) {
      double hist_normalized_0 = static_cast<double>(histogram[0][channel][i]) /
                                 static_cast<double>(width[0] * height[0]);
      double hist_normalized_1 = static_cast<double>(histogram[1][channel][i]) /
                                 static_cast<double>(width[1] * height[1]);
      diff_accum += hist_normalized_0 - hist_normalized_1;
      diff_total += std::fabs(diff_accum);
    }
    // Normalize to range 0..1
    double emd = diff_total / num_bins;
    max_emd = std::max(max_emd, emd);
  }

  if (max_emd >
      static_cast<double>(assert_similar_emd_histogram->GetTolerance())) {
    message_consumer_->Message(
        MessageConsumer::Severity::kError,
        assert_similar_emd_histogram->GetStartToken(),
        "Histogram EMD value of " + std::to_string(max_emd) +
            " is greater than tolerance of " +
            std::to_string(assert_similar_emd_histogram->GetTolerance()));
  }
  return true;
}

bool Executor::VisitBindSampler(CommandBindSampler* bind_sampler) {
  GL_SAFECALL(glBindSampler,
              static_cast<GLuint>(bind_sampler->GetTextureUnit()),
              created_samplers_.at(bind_sampler->GetSamplerIdentifier()));
  return true;
}

bool Executor::VisitBindStorageBuffer(
    CommandBindStorageBuffer* bind_storage_buffer) {
  GL_SAFECALL(
      glBindBufferBase, GL_SHADER_STORAGE_BUFFER,
      static_cast<GLuint>(bind_storage_buffer->GetBinding()),
      created_buffers_.at(bind_storage_buffer->GetStorageBufferIdentifier()));
  return true;
}

bool Executor::VisitBindTexture(CommandBindTexture* bind_texture) {
  GL_SAFECALL(
      glActiveTexture,
      GL_TEXTURE0 + static_cast<GLenum>(bind_texture->GetTextureUnit()));
  GL_SAFECALL(glBindTexture, GL_TEXTURE_2D,
              created_textures_.at(bind_texture->GetTextureIdentifier()));
  return true;
}

bool Executor::VisitBindUniformBuffer(
    CommandBindUniformBuffer* bind_uniform_buffer) {
  GL_SAFECALL(
      glBindBufferBase, GL_UNIFORM_BUFFER,
      static_cast<GLuint>(bind_uniform_buffer->GetBinding()),
      created_buffers_.at(bind_uniform_buffer->GetUniformBufferIdentifier()));
  return true;
}

bool Executor::VisitCompileShader(CommandCompileShader* compile_shader) {
  assert(declared_shaders_.count(compile_shader->GetShaderIdentifier()) == 1 &&
         "Shader not declared.");
  assert(compiled_shaders_.count(compile_shader->GetResultIdentifier()) == 0 &&
         "Identifier already in use for compiled shader.");
  CommandDeclareShader* shader_declaration =
      declared_shaders_.at(compile_shader->GetShaderIdentifier());
  GLenum shader_kind = GL_NONE;
  switch (shader_declaration->GetKind()) {
    case CommandDeclareShader::Kind::VERTEX:
      shader_kind = GL_VERTEX_SHADER;
      break;
    case CommandDeclareShader::Kind::FRAGMENT:
      shader_kind = GL_FRAGMENT_SHADER;
      break;
    case CommandDeclareShader::Kind::COMPUTE:
      shader_kind = GL_COMPUTE_SHADER;
      break;
  }
  GLuint shader = glCreateShader(shader_kind);
  GL_CHECKERR("glCreateShader");
  const char* temp = shader_declaration->GetShaderText().c_str();
  GL_SAFECALL(glShaderSource, shader, 1, &temp, nullptr);
  GL_SAFECALL(glCompileShader, shader);
  GLint status = 0;
  GL_SAFECALL(glGetShaderiv, shader, GL_COMPILE_STATUS, &status);
  if (status == 0) {
    PrintShaderError(shader);
    errcode_crash(COMPILE_ERROR_EXIT_CODE, "Shader compilation failed");
  }
  compiled_shaders_.insert({compile_shader->GetResultIdentifier(), shader});
  return true;
}

bool Executor::VisitCreateBuffer(CommandCreateBuffer* create_buffer) {
  GLuint buffer;
  GL_SAFECALL(glGenBuffers, 1, &buffer);
  // We arbitrarily bind to the ARRAY_BUFFER target.
  GL_SAFECALL(glBindBuffer, GL_ARRAY_BUFFER, buffer);
  if (create_buffer->HasInitialData()) {
    GL_SAFECALL(glBufferData, GL_ARRAY_BUFFER,
                static_cast<GLuint>(create_buffer->GetSizeBytes()),
                create_buffer->GetInitialData().data(), GL_STREAM_DRAW);
  } else {
    GL_SAFECALL(glBufferData, GL_ARRAY_BUFFER,
                static_cast<GLuint>(create_buffer->GetSizeBytes()), nullptr,
                GL_STREAM_DRAW);
  }
  created_buffers_.insert({create_buffer->GetResultIdentifier(), buffer});
  return true;
}

bool Executor::VisitCreateSampler(CommandCreateSampler* create_sampler) {
  GLuint sampler;
  GL_SAFECALL(glGenSamplers, 1, &sampler);
  created_samplers_.insert({create_sampler->GetResultIdentifier(), sampler});
  return true;
}

bool Executor::VisitCreateEmptyTexture2D(
    CommandCreateEmptyTexture2D* create_empty_texture_2d) {
  GLuint texture;
  GL_SAFECALL(glGenTextures, 1, &texture);
  GL_SAFECALL(glBindTexture, GL_TEXTURE_2D, texture);
  GL_SAFECALL(glTexImage2D, GL_TEXTURE_2D, 0, GL_RGBA,
              static_cast<GLsizei>(create_empty_texture_2d->GetWidth()),
              static_cast<GLsizei>(create_empty_texture_2d->GetHeight()), 0,
              GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  created_textures_.insert(
      {create_empty_texture_2d->GetResultIdentifier(), texture});
  return true;
}

bool Executor::VisitCreateProgram(CommandCreateProgram* create_program) {
  assert(created_programs_.count(create_program->GetResultIdentifier()) == 0 &&
         "Identifier already in use for created program.");
  GLuint program = glCreateProgram();
  GL_CHECKERR("glCreateProgram");
  if (program == 0) {
    crash("glCreateProgram()");
  }
  for (size_t index = 0; index < create_program->GetNumCompiledShaders();
       index++) {
    assert(compiled_shaders_.count(
               create_program->GetCompiledShaderIdentifier(index)) == 1 &&
           "Compiled shader not found.");
    GL_SAFECALL(glAttachShader, program,
                compiled_shaders_.at(
                    create_program->GetCompiledShaderIdentifier(index)));
  }
  GL_SAFECALL(glLinkProgram, program);
  GLint status = 0;
  GL_SAFECALL(glGetProgramiv, program, GL_LINK_STATUS, &status);
  if (status == 0) {
    PrintProgramError(program);
    errcode_crash(LINK_ERROR_EXIT_CODE, "Program linking failed");
  }
  created_programs_.insert({create_program->GetResultIdentifier(), program});
  return true;
}

bool Executor::VisitCreateRenderbuffer(
    CommandCreateRenderbuffer* create_renderbuffer) {
  GLuint render_buffer;
  GL_SAFECALL(glGenRenderbuffers, 1, &render_buffer);
  GL_SAFECALL(glBindRenderbuffer, GL_RENDERBUFFER, render_buffer);

  GL_SAFECALL(glRenderbufferStorage, GL_RENDERBUFFER, GL_RGBA8,
              static_cast<GLsizei>(create_renderbuffer->GetWidth()),
              static_cast<GLsizei>(create_renderbuffer->GetHeight()));
  created_renderbuffers_.insert(
      {create_renderbuffer->GetResultIdentifier(), render_buffer});
  return true;
}

bool Executor::VisitDeclareShader(CommandDeclareShader* declare_shader) {
  assert(declared_shaders_.count(declare_shader->GetResultIdentifier()) == 0 &&
         "Shader with this name already declared.");
  declared_shaders_.insert(
      {declare_shader->GetResultIdentifier(), declare_shader});
  return true;
}

bool Executor::VisitDumpRenderbuffer(
    CommandDumpRenderbuffer* dump_renderbuffer) {
  GLuint framebuffer_object_id;
  GL_SAFECALL(glGenFramebuffers, 1, &framebuffer_object_id);
  GL_SAFECALL(glBindFramebuffer, GL_FRAMEBUFFER, framebuffer_object_id);
  GL_SAFECALL(glFramebufferRenderbuffer, GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
              GL_RENDERBUFFER,
              created_renderbuffers_.at(
                  dump_renderbuffer->GetRenderbufferIdentifier()));
  size_t width;
  size_t height;
  {
    GLint temp_width;
    GL_SAFECALL(glGetRenderbufferParameteriv, GL_RENDERBUFFER,
                GL_RENDERBUFFER_WIDTH, &temp_width);
    GLint temp_height;
    GL_SAFECALL(glGetRenderbufferParameteriv, GL_RENDERBUFFER,
                GL_RENDERBUFFER_HEIGHT, &temp_height);
    width = static_cast<size_t>(temp_width);
    height = static_cast<size_t>(temp_height);
  }

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    crash(
        "Problem with OpenGL framebuffer after specifying color render buffer: "
        "n%xn",
        status);
  }

  std::vector<std::uint8_t> data(width * height * CHANNELS);
  GL_SAFECALL(glReadBuffer, GL_COLOR_ATTACHMENT0);
  GL_SAFECALL(glReadPixels, 0, 0, static_cast<GLint>(width),
              static_cast<GLint>(height), GL_RGBA, GL_UNSIGNED_BYTE,
              data.data());
  std::vector<std::uint8_t> flipped_data(width * height * CHANNELS);
  for (size_t h = 0; h < height; h++) {
    for (size_t col = 0; col < width * CHANNELS; col++) {
      flipped_data[h * width * CHANNELS + col] =
          data[(height - h - 1) * width * CHANNELS + col];
    }
  }
  unsigned png_error = lodepng::encode(
      dump_renderbuffer->GetFilename(), flipped_data,
      static_cast<unsigned int>(width), static_cast<unsigned int>(height));
  if (png_error != 0) {
    crash("lodepng: %s", lodepng_error_text(png_error));
  }
  GL_SAFECALL(glDeleteFramebuffers, 1, &framebuffer_object_id);
  return true;
}

bool Executor::VisitRunCompute(CommandRunCompute* run_compute) {
  GL_SAFECALL(glMemoryBarrier, GL_ALL_BARRIER_BITS);

  GL_SAFECALL(glUseProgram,
              created_programs_.at(run_compute->GetProgramIdentifier()));

  GL_SAFECALL(glDispatchCompute,
              static_cast<GLuint>(run_compute->GetNumGroupsX()),
              static_cast<GLuint>(run_compute->GetNumGroupsY()),
              static_cast<GLuint>(run_compute->GetNumGroupsZ()));

  GL_SAFECALL_NO_ARGS(glFlush);

  return true;
}

bool Executor::VisitRunGraphics(CommandRunGraphics* run_graphics) {
  GL_SAFECALL(glMemoryBarrier, GL_ALL_BARRIER_BITS);

  auto vertex_data = run_graphics->GetVertexData();
  for (const auto& entry : vertex_data) {
    GL_SAFECALL(glBindBuffer, GL_ARRAY_BUFFER,
                created_buffers_.at(entry.second.GetBufferIdentifier()));
    GL_SAFECALL(glEnableVertexAttribArray, static_cast<GLuint>(entry.first));
    GL_SAFECALL(glVertexAttribPointer, static_cast<GLuint>(entry.first),
                static_cast<GLsizei>(entry.second.GetDimension()), GL_FLOAT,
                GL_FALSE, static_cast<GLsizei>(entry.second.GetStrideBytes()),
                reinterpret_cast<void*>(entry.second.GetOffsetBytes()));
  }

  GL_SAFECALL(glUseProgram,
              created_programs_.at(run_graphics->GetProgramIdentifier()));

  GLuint framebuffer_object_id;
  GL_SAFECALL(glGenFramebuffers, 1, &framebuffer_object_id);
  GL_SAFECALL(glBindFramebuffer, GL_FRAMEBUFFER, framebuffer_object_id);

  auto framebuffer_attachments = run_graphics->GetFramebufferAttachments();
  assert(framebuffer_attachments.size() <= 32 && "Too many renderbuffers.");
  size_t max_location = 0;
  for (const auto& entry : framebuffer_attachments) {
    max_location = std::max(max_location, entry.first);
  }
  std::vector<GLenum> draw_buffers;
  for (size_t i = 0; i <= max_location; i++) {
    if (framebuffer_attachments.count(i) > 0) {
      GLenum color_attachment = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
      auto output_buffer = framebuffer_attachments.at(i);
      if (created_renderbuffers_.count(output_buffer) != 0) {
        GL_SAFECALL(glFramebufferRenderbuffer, GL_FRAMEBUFFER, color_attachment,
                    GL_RENDERBUFFER,
                    created_renderbuffers_.at(framebuffer_attachments.at(i)));
      } else {
        GL_SAFECALL(glFramebufferTexture, GL_FRAMEBUFFER, color_attachment,
                    created_textures_.at(framebuffer_attachments.at(i)), 0);
      }
      draw_buffers.push_back(color_attachment);
    } else {
      draw_buffers.push_back(GL_NONE);
    }
  }

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    crash(
        "Problem with OpenGL framebuffer after specifying color render buffer: "
        "n%xn",
        status);
  }

  GL_SAFECALL(glDrawBuffers, static_cast<GLsizei>(draw_buffers.size()),
              draw_buffers.data());

  GL_SAFECALL(glClearColor, 0.0F, 0.0F, 0.0F, 1.0F);
  GL_SAFECALL(glClear, GL_COLOR_BUFFER_BIT);

  GL_SAFECALL(
      glBindBuffer, GL_ELEMENT_ARRAY_BUFFER,
      created_buffers_.at(run_graphics->GetIndexDataBufferIdentifier()));
  GLenum topology = GL_NONE;
  switch (run_graphics->GetTopology()) {
    case CommandRunGraphics::Topology::kTriangles:
      topology = GL_TRIANGLES;
      break;
  }
  GL_SAFECALL(glDrawElements, topology,
              static_cast<GLsizei>(run_graphics->GetVertexCount()),
              GL_UNSIGNED_INT, reinterpret_cast<GLvoid*>(0));

  GL_SAFECALL_NO_ARGS(glFlush);

  for (const auto& entry : run_graphics->GetVertexData()) {
    GL_SAFECALL(glDisableVertexAttribArray, static_cast<GLuint>(entry.first));
  }

  GL_SAFECALL(glDeleteFramebuffers, 1, &framebuffer_object_id);
  return true;
}

bool Executor::VisitSetSamplerOrTextureParameter(
    CommandSetSamplerOrTextureParameter* set_sampler_or_texture_parameter) {
  GLenum parameter = GL_NONE;
  switch (set_sampler_or_texture_parameter->GetParameter()) {
    case CommandSetSamplerOrTextureParameter::TextureParameter::kMagFilter:
      parameter = GL_TEXTURE_MAG_FILTER;
      break;
    case CommandSetSamplerOrTextureParameter::TextureParameter::kMinFilter:
      parameter = GL_TEXTURE_MIN_FILTER;
      break;
  }
  GLint parameter_value = GL_NONE;
  switch (set_sampler_or_texture_parameter->GetParameterValue()) {
    case CommandSetSamplerOrTextureParameter::TextureParameterValue::kNearest:
      parameter_value = GL_NEAREST;
      break;
    case CommandSetSamplerOrTextureParameter::TextureParameterValue::kLinear:
      parameter_value = GL_LINEAR;
      break;
  }
  if (created_samplers_.count(
          set_sampler_or_texture_parameter->GetTargetTextureOrSampler()) > 0) {
    GL_SAFECALL(
        glSamplerParameteri,
        created_samplers_.at(
            set_sampler_or_texture_parameter->GetTargetTextureOrSampler()),
        parameter, parameter_value);
  } else {
    assert(created_textures_.count(
               set_sampler_or_texture_parameter->GetTargetTextureOrSampler()) >
               0 &&
           "Unknown texture or sampler.");
    GL_SAFECALL(
        glBindTexture, GL_TEXTURE_2D,
        created_textures_.at(
            set_sampler_or_texture_parameter->GetTargetTextureOrSampler()));
    GL_SAFECALL(glTexParameteri, GL_TEXTURE_2D, parameter, parameter_value);
  }
  return true;
}

bool Executor::VisitSetUniform(CommandSetUniform* set_uniform) {
  GLuint program = created_programs_.at(set_uniform->GetProgramIdentifier());
  auto uniform_location = static_cast<GLint>(set_uniform->GetLocation());
  const UniformValue& uniform_value = set_uniform->GetValue();
  switch (uniform_value.GetElementType()) {
    case UniformValue::ElementType::kFloat:
      if (uniform_value.IsArray()) {
        GL_SAFECALL(glProgramUniform1fv, program, uniform_location,
                    static_cast<GLint>(uniform_value.GetArraySize()),
                    uniform_value.GetFloatData());
      } else {
        GL_SAFECALL(glProgramUniform1f, program, uniform_location,
                    uniform_value.GetFloatData()[0]);
      }
      break;
    case UniformValue::ElementType::kVec2:
      if (uniform_value.IsArray()) {
        GL_SAFECALL(glProgramUniform2fv, program, uniform_location,
                    static_cast<GLsizei>(uniform_value.GetArraySize()),
                    uniform_value.GetFloatData());
      } else {
        GL_SAFECALL(glProgramUniform2f, program, uniform_location,
                    uniform_value.GetFloatData()[0],
                    uniform_value.GetFloatData()[1]);
      }
      break;
    case UniformValue::ElementType::kVec3:
      if (uniform_value.IsArray()) {
        GL_SAFECALL(glProgramUniform3fv, program, uniform_location,
                    static_cast<GLsizei>(uniform_value.GetArraySize()),
                    uniform_value.GetFloatData());
      } else {
        GL_SAFECALL(glProgramUniform3f, program, uniform_location,
                    uniform_value.GetFloatData()[0],
                    uniform_value.GetFloatData()[1],
                    uniform_value.GetFloatData()[2]);
      }
      break;
    case UniformValue::ElementType::kVec4:
      if (uniform_value.IsArray()) {
        GL_SAFECALL(glProgramUniform4fv, program, uniform_location,
                    static_cast<GLsizei>(uniform_value.GetArraySize()),
                    uniform_value.GetFloatData());
      } else {
        GL_SAFECALL(
            glProgramUniform4f, program, uniform_location,
            uniform_value.GetFloatData()[0], uniform_value.GetFloatData()[1],
            uniform_value.GetFloatData()[2], uniform_value.GetFloatData()[3]);
      }
      break;
    case UniformValue::ElementType::kInt:
      if (uniform_value.IsArray()) {
        GL_SAFECALL(glProgramUniform1iv, program, uniform_location,
                    static_cast<GLsizei>(uniform_value.GetArraySize()),
                    uniform_value.GetIntData());
      } else {
        GL_SAFECALL(glProgramUniform1i, program, uniform_location,
                    uniform_value.GetIntData()[0]);
      }
      break;
    default:
      assert(false && "Unhandled uniform type.");
      break;
  }
  return true;
}

bool Executor::CheckEqualRenderbuffers(CommandAssertEqual* assert_equal) {
  assert(created_renderbuffers_.count(assert_equal->GetBufferIdentifier1()) !=
             0 &&
         "Expected a renderbuffer");
  assert(created_renderbuffers_.count(assert_equal->GetBufferIdentifier2()) !=
             0 &&
         "Expected a renderbuffer");

  GLuint renderbuffers[2];
  renderbuffers[0] =
      created_renderbuffers_.at(assert_equal->GetBufferIdentifier1());
  renderbuffers[1] =
      created_renderbuffers_.at(assert_equal->GetBufferIdentifier2());

  size_t width[2] = {0, 0};
  size_t height[2] = {0, 0};

  for (auto index : {0, 1}) {
    {
      GLint temp_width;
      GL_SAFECALL(glBindRenderbuffer, GL_RENDERBUFFER, renderbuffers[index]);
      GL_SAFECALL(glGetRenderbufferParameteriv, GL_RENDERBUFFER,
                  GL_RENDERBUFFER_WIDTH, &temp_width);
      width[index] = static_cast<size_t>(temp_width);
    }
    {
      GLint temp_height;
      GL_SAFECALL(glGetRenderbufferParameteriv, GL_RENDERBUFFER,
                  GL_RENDERBUFFER_HEIGHT, &temp_height);
      height[index] = static_cast<size_t>(temp_height);
    }
  }

  if (width[0] != width[1]) {
    std::stringstream stringstream;
    stringstream << "The widths of " << assert_equal->GetBufferIdentifier1()
                 << " and " << assert_equal->GetBufferIdentifier2()
                 << " do not match: " << width[0] << " vs. " << width[1];
    message_consumer_->Message(MessageConsumer::Severity::kError,
                               assert_equal->GetStartToken(),
                               stringstream.str());
    return false;
  }

  if (height[0] != height[1]) {
    std::stringstream stringstream;
    stringstream << "The heights of " << assert_equal->GetBufferIdentifier1()
                 << " and " << assert_equal->GetBufferIdentifier2()
                 << " do not match: " << height[0] << " vs. " << height[1];
    message_consumer_->Message(MessageConsumer::Severity::kError,
                               assert_equal->GetStartToken(),
                               stringstream.str());
    return false;
  }

  GLuint framebuffer_object_id;
  GL_SAFECALL(glGenFramebuffers, 1, &framebuffer_object_id);
  GL_SAFECALL(glBindFramebuffer, GL_FRAMEBUFFER, framebuffer_object_id);
  for (auto index : {0, 1}) {
    GL_SAFECALL(glFramebufferRenderbuffer, GL_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(index),
                GL_RENDERBUFFER, renderbuffers[index]);
  }
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    crash(
        "Problem with OpenGL framebuffer after specifying color render buffer: "
        "n%xn",
        status);
  }

  std::vector<std::uint8_t> data[2];
  for (auto index : {0, 1}) {
    data[index].resize(width[index] * height[index] * CHANNELS);
    GL_SAFECALL(glReadBuffer,
                GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(index));
    GL_SAFECALL(glReadPixels, 0, 0, static_cast<GLsizei>(width[index]),
                static_cast<GLsizei>(height[index]), GL_RGBA, GL_UNSIGNED_BYTE,
                data[index].data());
  }

  bool result = true;
  for (size_t y = 0; y < static_cast<size_t>(height[0]); y++) {
    for (size_t x = 0; x < static_cast<size_t>(width[0]); x++) {
      size_t offset = (static_cast<size_t>(height[0]) - y - 1) *
                          static_cast<size_t>(width[0]) * 4 +
                      x * 4;
      bool all_match = true;
      for (size_t component = 0; component < 4; component++) {
        if (data[0][offset + component] != data[1][offset + component]) {
          all_match = false;
          break;
        }
      }
      if (!all_match) {
        std::stringstream stringstream;
        stringstream << "Pixel mismatch at position (" << x << ", " << y
                     << "): " << assert_equal->GetBufferIdentifier1() << "["
                     << x << "][" << y << "] == ("
                     << static_cast<uint32_t>(data[0][offset]) << ", "
                     << static_cast<uint32_t>(data[0][offset + 1]) << ", "
                     << static_cast<uint32_t>(data[0][offset + 2]) << ", "
                     << static_cast<uint32_t>(data[0][offset + 3]) << "), vs. "
                     << assert_equal->GetBufferIdentifier2() << "[" << x << "]["
                     << y << "] == (" << static_cast<uint32_t>(data[1][offset])
                     << ", " << static_cast<uint32_t>(data[1][offset + 1])
                     << ", " << static_cast<uint32_t>(data[1][offset + 2])
                     << ", " << static_cast<uint32_t>(data[1][offset + 3])
                     << ")";
        message_consumer_->Message(MessageConsumer::Severity::kError,
                                   assert_equal->GetStartToken(),
                                   stringstream.str());
        result = false;
      }
    }
  }
  return result;
}

bool Executor::CheckEqualBuffers(CommandAssertEqual* assert_equal) {
  assert(created_buffers_.count(assert_equal->GetBufferIdentifier1()) != 0 &&
         "Expected a buffer");
  assert(created_buffers_.count(assert_equal->GetBufferIdentifier2()) != 0 &&
         "Expected a buffer");

  GLuint buffers[2];
  buffers[0] = created_buffers_.at(assert_equal->GetBufferIdentifier1());
  buffers[1] = created_buffers_.at(assert_equal->GetBufferIdentifier2());

  GLint64 buffer_size[2]{0, 0};
  for (auto index : {0, 1}) {
    GL_SAFECALL(glBindBuffer, GL_ARRAY_BUFFER, buffers[index]);
    GL_SAFECALL(glGetBufferParameteri64v, GL_ARRAY_BUFFER, GL_BUFFER_SIZE,
                &buffer_size[index]);
  }

  if (buffer_size[0] != buffer_size[1]) {
    std::stringstream stringstream;
    stringstream << "The lengths of " << assert_equal->GetBufferIdentifier1()
                 << " and " << assert_equal->GetBufferIdentifier2()
                 << " do not match: " << buffer_size[0] << " vs. "
                 << buffer_size[1];
    message_consumer_->Message(MessageConsumer::Severity::kError,
                               assert_equal->GetStartToken(),
                               stringstream.str());
    return false;
  }

  uint8_t* mapped_buffer[2]{nullptr, nullptr};
  for (auto index : {0, 1}) {
    GL_SAFECALL(glBindBuffer, GL_ARRAY_BUFFER, buffers[index]);
    mapped_buffer[index] = static_cast<uint8_t*>(glMapBufferRange(
        GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(buffer_size[index]),
        GL_MAP_READ_BIT));
    if (mapped_buffer[index] == nullptr) {
      // TODO(afd): If index == 1 should we unmap buffers[0] before returning?
      // Or are we OK so long as we eventually destroy that buffer?
      GL_CHECKERR("glMapBufferRange");
      return false;
    }
  }

  bool result = true;
  for (size_t index = 0; index < static_cast<size_t>(buffer_size[0]); index++) {
    // We only get here if the calls to glMapBufferRange succeeded, in which
    // case the contents of |mapped_buffer| cannot be null.
    uint8_t value_1 =
        mapped_buffer[0][index];  // NOLINT(clang-analyzer-core.NullDereference)
    uint8_t value_2 =
        mapped_buffer[1][index];  // NOLINT(clang-analyzer-core.NullDereference)
    if (value_1 != value_2) {
      std::stringstream stringstream;
      stringstream << "Byte mismatch at index " << index << ": "
                   << assert_equal->GetBufferIdentifier1() << "[" << index
                   << "] == " << static_cast<uint32_t>(value_1) << ", "
                   << assert_equal->GetBufferIdentifier2() << "[" << index
                   << "] == " << static_cast<uint32_t>(value_2);
      message_consumer_->Message(MessageConsumer::Severity::kError,
                                 assert_equal->GetStartToken(),
                                 stringstream.str());
      result = false;
    }
  }

  for (auto index : {0, 1}) {
    GL_SAFECALL(glBindBuffer, GL_ARRAY_BUFFER, buffers[index]);
    GL_SAFECALL(glUnmapBuffer, GL_ARRAY_BUFFER);
  }
  return result;
}

}  // namespace shadertrap
