// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "media/gpu/omx/omx_video_decode_accelerator.h"

#include "base/bind.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/trace_event/trace_event.h"
#include "media/base/bitstream_buffer.h"
#include "media/video/picture.h"

#include "media/gpu/omx/omx_stubs.h"

using media_gpu_omx::kModuleOmx;
using media_gpu_omx::InitializeStubs;
using media_gpu_omx::StubPathMap;

static const base::FilePath::CharType kOMXLib[] =
    FILE_PATH_LITERAL("/usr/lib/libomxr_core.so");

namespace media {

// Helper typedef for input buffers.  This is used as the pAppPrivate field of
// OMX_BUFFERHEADERTYPEs of input buffers, to point to the data associated with
// them.
typedef std::pair<std::unique_ptr<base::SharedMemory>, int32_t> SharedMemoryAndId;

enum { kNumPictureBuffers = 8 };

// Delay between polling for texture sync status. 5ms feels like a good
// compromise, allowing some decoding ahead (up to 3 frames/vsync) to compensate
// for more difficult frames.
enum { kSyncPollDelayMs = 5 };

// Maps h264-related Profile enum values to OMX_VIDEO_AVCPROFILETYPE values.
static OMX_U32 MapH264ProfileToOMXAVCProfile(uint32_t profile) {
  switch (profile) {
    case media::H264PROFILE_BASELINE:
      return OMX_VIDEO_AVCProfileBaseline;
    case media::H264PROFILE_MAIN:
      return OMX_VIDEO_AVCProfileMain;
    case media::H264PROFILE_EXTENDED:
      return OMX_VIDEO_AVCProfileExtended;
    case media::H264PROFILE_HIGH:
      return OMX_VIDEO_AVCProfileHigh;
    case media::H264PROFILE_HIGH10PROFILE:
      return OMX_VIDEO_AVCProfileHigh10;
    case media::H264PROFILE_HIGH422PROFILE:
      return OMX_VIDEO_AVCProfileHigh422;
    case media::H264PROFILE_HIGH444PREDICTIVEPROFILE:
      return OMX_VIDEO_AVCProfileHigh444;
    // Below enums don't have equivalent enum in Openmax.
    case media::H264PROFILE_SCALABLEBASELINE:
    case media::H264PROFILE_SCALABLEHIGH:
    case media::H264PROFILE_STEREOHIGH:
    case media::H264PROFILE_MULTIVIEWHIGH:
      // Nvidia OMX video decoder requires the same resources (as that of the
      // High profile) in every profile higher to the Main profile.
      return OMX_VIDEO_AVCProfileHigh444;
    default:
      NOTREACHED();
      return OMX_VIDEO_AVCProfileMax;
  }
}

// Helper macros for dealing with failure.  If |result| evaluates false, emit
// |log| to ERROR, register |error| with the decoder, and return |ret_val|
// (which may be omitted for functions that return void).
#define RETURN_ON_FAILURE(result, log, error, ret_val)             \
  do {                                                             \
    if (!(result)) {                                               \
      DLOG(ERROR) << log;                                          \
      StopOnError(error);                                          \
      return ret_val;                                              \
    }                                                              \
  } while (0)

// OMX-specific version of RETURN_ON_FAILURE which compares with OMX_ErrorNone.
#define RETURN_ON_OMX_FAILURE(omx_result, log, error, ret_val)          \
  RETURN_ON_FAILURE(                                                    \
      ((omx_result) == OMX_ErrorNone),                                  \
      log << ", OMX result: 0x" << std::hex << omx_result,              \
      error, ret_val)

// static
bool OmxVideoDecodeAccelerator::pre_sandbox_init_done_ = false;

class OmxVideoDecodeAccelerator::PictureSyncObject {
 public:
  // Create a sync object and insert into the GPU command stream.
  PictureSyncObject(EGLDisplay egl_display);
  ~PictureSyncObject();

  bool IsSynced();

 private:
  EGLSyncKHR egl_sync_obj_;
  EGLDisplay egl_display_;
};

OmxVideoDecodeAccelerator::PictureSyncObject::PictureSyncObject(
    EGLDisplay egl_display)
    : egl_display_(egl_display) {
  DCHECK(egl_display_ != EGL_NO_DISPLAY);

  egl_sync_obj_ = eglCreateSyncKHR(egl_display_, EGL_SYNC_FENCE_KHR, NULL);
  DCHECK_NE(egl_sync_obj_, EGL_NO_SYNC_KHR);
}

OmxVideoDecodeAccelerator::PictureSyncObject::~PictureSyncObject() {
  eglDestroySyncKHR(egl_display_, egl_sync_obj_);
}

bool OmxVideoDecodeAccelerator::PictureSyncObject::IsSynced() {
  EGLint value = EGL_UNSIGNALED_KHR;
  EGLBoolean ret = eglGetSyncAttribKHR(
      egl_display_, egl_sync_obj_, EGL_SYNC_STATUS_KHR, &value);
  DCHECK(ret) << "Failed getting sync object state.";

  return value == EGL_SIGNALED_KHR;
}

OmxVideoDecodeAccelerator::OmxVideoDecodeAccelerator(
    EGLDisplay egl_display,
    const base::Callback<bool(void)>& make_context_current)
    : child_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      component_handle_(NULL),
      weak_this_factory_(this),
      init_begun_(false),
      client_state_(OMX_StateMax),
      current_state_change_(NO_TRANSITION),
      input_buffer_count_(0),
      input_buffer_size_(0),
      input_port_(0),
      input_buffers_at_component_(0),
      output_port_(0),
      output_buffers_at_component_(0),
      egl_display_(egl_display),
      make_context_current_(make_context_current),
      codec_(UNKNOWN),
      h264_profile_(OMX_VIDEO_AVCProfileMax),
      component_name_is_nvidia_(false) {
  weak_this_ = weak_this_factory_.GetWeakPtr();
  static bool omx_functions_initialized = PostSandboxInitialization();
  RETURN_ON_FAILURE(omx_functions_initialized,
                    "Failed to load openmax library", PLATFORM_FAILURE,);
  RETURN_ON_OMX_FAILURE(OMX_Init(), "Failed to init OpenMAX core",
                        PLATFORM_FAILURE,);
}

OmxVideoDecodeAccelerator::~OmxVideoDecodeAccelerator() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK(free_input_buffers_.empty());
  DCHECK_EQ(0, input_buffers_at_component_);
  DCHECK_EQ(0, output_buffers_at_component_);
  DCHECK(pictures_.empty());
}

// This is to initialize the OMX data structures to default values.
template <typename T>
static void InitParam(T* param) {
  memset(param, 0, sizeof(T));
  param->nVersion.nVersion = 0x00000101;
  param->nSize = sizeof(T);
}

static const VideoCodecProfile kSupportedProfiles[] = {
    H264PROFILE_BASELINE,
    H264PROFILE_MAIN,
    H264PROFILE_HIGH,
    VP8PROFILE_ANY
};

VideoDecodeAccelerator::SupportedProfiles
OmxVideoDecodeAccelerator::GetSupportedProfiles() {
    VideoDecodeAccelerator::SupportedProfiles profiles;

    for (const auto& profile : kSupportedProfiles) {
        const auto kMinSize = gfx::Size(16,16); //TODO; check this
        const auto kMaxSize = gfx::Size(1920,1080);
        VideoDecodeAccelerator::SupportedProfile supp_profile;
        supp_profile.profile = profile;
        supp_profile.min_resolution = kMinSize;
        supp_profile.max_resolution = kMaxSize;
        supp_profile.encrypted_only = false;
        profiles.push_back(supp_profile);
    }
    return profiles;
}

bool OmxVideoDecodeAccelerator::Initialize(const Config& config, Client* client) {
  auto profile = config.profile;
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  if (profile >= media::H264PROFILE_MIN && profile <= media::H264PROFILE_MAX) {
    codec_ = H264;
    h264_profile_ = MapH264ProfileToOMXAVCProfile(profile);
    RETURN_ON_FAILURE(h264_profile_ != OMX_VIDEO_AVCProfileMax,
                      "Unexpected profile", INVALID_ARGUMENT, false);
  } else if (profile >= media::VP8PROFILE_MIN && profile <= media::VP8PROFILE_MAX) {
    codec_ = VP8;
  } else {
    RETURN_ON_FAILURE(false, "Unsupported profile: " << profile,
                      INVALID_ARGUMENT, false);
  }

  // Make sure that we have a context we can use for EGL image binding.
  RETURN_ON_FAILURE(make_context_current_.Run(),
                    "Failed make context current",
                    PLATFORM_FAILURE,
                    false);

  client_ptr_factory_.reset(new base::WeakPtrFactory<Client>(client));
  client_ = client_ptr_factory_->GetWeakPtr();

// TODO: Check the config supported_output_formats to make sure that it matches what we can output

/* TODO: Is this a function call? How is this being checked? Does it work for
   OpenGL ES ?
  RETURN_ON_FAILURE(gfx::g_driver_egl.ext.b_EGL_KHR_fence_sync,
                    "Platform does not support EGL_KHR_fence_sync",
                    PLATFORM_FAILURE,
                    false);
*/

  if (!CreateComponent())  // Does its own RETURN_ON_FAILURE dances.
    return false;

  DCHECK_EQ(current_state_change_, NO_TRANSITION);
  current_state_change_ = INITIALIZING;
  BeginTransitionToState(OMX_StateIdle);

  if (!AllocateInputBuffers())  // Does its own RETURN_ON_FAILURE dances.
    return false;
  if (!AllocateFakeOutputBuffers())  // Does its own RETURN_ON_FAILURE dances.
    return false;

  init_begun_ = true;
  return true;
}

bool OmxVideoDecodeAccelerator::CreateComponent() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  OMX_CALLBACKTYPE omx_accelerator_callbacks = {
    &OmxVideoDecodeAccelerator::EventHandler,
    &OmxVideoDecodeAccelerator::EmptyBufferCallback,
    &OmxVideoDecodeAccelerator::FillBufferCallback
  };

  // TODO: Set to R-Car names
  OMX_STRING role_name = codec_ == H264 ?
      const_cast<OMX_STRING>("video_decoder.avc") :
      const_cast<OMX_STRING>("video_decoder.vpx");

  // Get the first component for this role and set the role on it.

  OMX_U32 num_components = 1;
  char component[OMX_MAX_STRINGNAME_SIZE];
  OMX_ERRORTYPE result = OMX_GetComponentsOfRole(
      role_name, &num_components,
      reinterpret_cast<OMX_U8**>(&component));
  RETURN_ON_OMX_FAILURE(result, "Unsupported role: " << role_name,
                        PLATFORM_FAILURE, false);
  RETURN_ON_FAILURE(num_components == 1, "No components for: " << role_name,
                    PLATFORM_FAILURE, false);

  // Get the handle to the component.
  result = OMX_GetHandle(
      &component_handle_,
      reinterpret_cast<OMX_STRING>(component),
      this, &omx_accelerator_callbacks);
  RETURN_ON_OMX_FAILURE(result,
                        "Failed to OMX_GetHandle on: " << component,
                        PLATFORM_FAILURE, false);
  client_state_ = OMX_StateLoaded;

  // Get the port information. This will obtain information about the number of
  // ports and index of the first port.
  OMX_PORT_PARAM_TYPE port_param;
  InitParam(&port_param);
  result = OMX_GetParameter(component_handle_, OMX_IndexParamVideoInit,
                            &port_param);
  RETURN_ON_FAILURE(result == OMX_ErrorNone && port_param.nPorts == 2,
                    "Failed to get Port Param: " << result << ", "
                    << port_param.nPorts,
                    PLATFORM_FAILURE, false);

  input_port_ = port_param.nStartPortNumber;
  //TODO: Is this true?
  output_port_ = input_port_ + 1;

  // Set role for the component because components can have multiple roles.
  // TODO: Maybe unnecessary, but leave this in if it works
  OMX_PARAM_COMPONENTROLETYPE role_type;
  InitParam(&role_type);
  base::strlcpy(reinterpret_cast<char*>(role_type.cRole),
                role_name,
                OMX_MAX_STRINGNAME_SIZE);

  result = OMX_SetParameter(component_handle_,
                            OMX_IndexParamStandardComponentRole,
                            &role_type);
  RETURN_ON_OMX_FAILURE(result, "Failed to Set Role",
                        PLATFORM_FAILURE, false);

  // Populate input-buffer-related members based on input port data.
  OMX_PARAM_PORTDEFINITIONTYPE port_format;
  InitParam(&port_format);
  port_format.nPortIndex = input_port_;
  result = OMX_GetParameter(component_handle_,
                            OMX_IndexParamPortDefinition,
                            &port_format);
  RETURN_ON_OMX_FAILURE(result,
                        "GetParameter(OMX_IndexParamPortDefinition) failed",
                        PLATFORM_FAILURE, false);
  RETURN_ON_FAILURE(OMX_DirInput == port_format.eDir, "Expected input port",
                    PLATFORM_FAILURE, false);

  input_buffer_count_ = port_format.nBufferCountActual;
  input_buffer_size_ = port_format.nBufferSize;

  // Verify output port conforms to our expectations.
  InitParam(&port_format);
  port_format.nPortIndex = output_port_;
  result = OMX_GetParameter(component_handle_,
                            OMX_IndexParamPortDefinition,
                            &port_format);
  RETURN_ON_OMX_FAILURE(result,
                        "GetParameter(OMX_IndexParamPortDefinition) failed",
                        PLATFORM_FAILURE, false);
  RETURN_ON_FAILURE(OMX_DirOutput == port_format.eDir, "Expect Output Port",
                    PLATFORM_FAILURE, false);

  // Set output port parameters.
  port_format.nBufferCountActual = kNumPictureBuffers;
  
  // Force an OMX_EventPortSettingsChanged event to be sent once we know the
  // stream's real dimensions (which can only happen once some Decode() work has
  // been done).
  // TODO: Agreed, but is this how R-Car does it?
  port_format.format.video.nFrameWidth = -1;
  port_format.format.video.nFrameHeight = -1;
  result = OMX_SetParameter(component_handle_,
                            OMX_IndexParamPortDefinition,
                            &port_format);
  RETURN_ON_OMX_FAILURE(result,
                        "SetParameter(OMX_IndexParamPortDefinition) failed",
                        PLATFORM_FAILURE, false);
  return true;
}

void OmxVideoDecodeAccelerator::Decode(
    const media::BitstreamBuffer& bitstream_buffer) {
  TRACE_EVENT1("Video Decoder", "OVDA::Decode",
               "Buffer id", bitstream_buffer.id());
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  if (current_state_change_ == RESETTING ||
      current_state_change_ == INITIALIZING ||
      !queued_bitstream_buffers_.empty() ||
      free_input_buffers_.empty()) {
    queued_bitstream_buffers_.push_back(bitstream_buffer);
    return;
  }

  RETURN_ON_FAILURE((current_state_change_ == NO_TRANSITION ||
                     current_state_change_ == FLUSHING) &&
                    (client_state_ == OMX_StateIdle ||
                     client_state_ == OMX_StateExecuting),
                    "Call to Decode() during invalid state or transition: "
                    << current_state_change_ << ", " << client_state_,
                    ILLEGAL_STATE,);

  OMX_BUFFERHEADERTYPE* omx_buffer = free_input_buffers_.front();
  free_input_buffers_.pop();

  if (bitstream_buffer.id() == -1 && bitstream_buffer.size() == 0) {
    // Cook up an empty buffer w/ EOS set and feed it to OMX.
    omx_buffer->nFilledLen = 0;
    omx_buffer->nAllocLen = omx_buffer->nFilledLen;
    omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;
    omx_buffer->nTimeStamp = -2;
    OMX_ERRORTYPE result = OMX_EmptyThisBuffer(component_handle_, omx_buffer);
    RETURN_ON_OMX_FAILURE(result, "OMX_EmptyThisBuffer() failed",
                          PLATFORM_FAILURE,);
    input_buffers_at_component_++;
    return;
  }

  // Setup |omx_buffer|.
  std::unique_ptr<base::SharedMemory> shm(
      new base::SharedMemory(bitstream_buffer.handle(), true));
  RETURN_ON_FAILURE(shm->Map(bitstream_buffer.size()),
                    "Failed to SharedMemory::Map()", UNREADABLE_INPUT,);

  //TODO: Set up more appropriate pAppPrivate data

  SharedMemoryAndId* input_buffer_details = new SharedMemoryAndId();

  //TODO: What is this "release" doing?

  input_buffer_details->first.reset(shm.release());
  input_buffer_details->second = bitstream_buffer.id();
  DCHECK(!omx_buffer->pAppPrivate);
  omx_buffer->pAppPrivate = input_buffer_details;
  omx_buffer->pBuffer =
      static_cast<OMX_U8*>(input_buffer_details->first->memory());
  omx_buffer->nFilledLen = bitstream_buffer.size();
  omx_buffer->nAllocLen = omx_buffer->nFilledLen;
  omx_buffer->nFlags &= ~OMX_BUFFERFLAG_EOS;
  // Abuse the header's nTimeStamp field to propagate the bitstream buffer ID to
  // the output buffer's nTimeStamp field, so we can report it back to the
  // client in PictureReady().
  omx_buffer->nTimeStamp = bitstream_buffer.id();

  // Give this buffer to OMX.
  OMX_ERRORTYPE result = OMX_EmptyThisBuffer(component_handle_, omx_buffer);
  RETURN_ON_OMX_FAILURE(result, "OMX_EmptyThisBuffer() failed",
                        PLATFORM_FAILURE,);
  input_buffers_at_component_++;
}

void OmxVideoDecodeAccelerator::AssignPictureBuffers(
    const std::vector<media::PictureBuffer>& buffers) {
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  // If we are resetting/destroying/erroring, don't bother, as
  // OMX_FillThisBuffer will fail anyway. In case we're in the middle of
  // closing, this will put the Accelerator in ERRORING mode, which has the
  // unwanted side effect of not going through the OMX_FreeBuffers path and
  // leaks memory.
  if (current_state_change_ == RESETTING ||
      current_state_change_ == DESTROYING ||
      current_state_change_ == ERRORING)
    return;

  RETURN_ON_FAILURE(CanFillBuffer(), "Can't fill buffer", ILLEGAL_STATE,);
  RETURN_ON_FAILURE((kNumPictureBuffers == buffers.size()),
      "Failed to provide requested picture buffers. (Got " << buffers.size() <<
      ", requested " << kNumPictureBuffers << ")", INVALID_ARGUMENT,);

  DCHECK_EQ(output_buffers_at_component_, 0);
  DCHECK_EQ(fake_output_buffers_.size(), 0U);
  DCHECK_EQ(pictures_.size(), 0U);

  if (!make_context_current_.Run())
    return;

  for (size_t i = 0; i < buffers.size(); ++i) {
    /*TODO: Allocate a new buffer from MMNGR, and create an EGLImage
            from it. Make DMABufs if necessary */

    /*
    EGLImageKHR egl_image =
        texture_to_egl_image_translator_->TranslateToEglImage(
            egl_display_, egl_context_,
            buffers[i].texture_id(),
            last_requested_picture_buffer_dimensions_); 
    CHECK(pictures_.insert(std::make_pair(
        buffers[i].id(), OutputPicture(buffers[i], NULL, egl_image))).second); */
  }

  // These do their own RETURN_ON_FAILURE dances.
  if (!AllocateOutputBuffers())
    return;
  if (!SendCommandToPort(OMX_CommandPortEnable, output_port_))
    return;
}

void OmxVideoDecodeAccelerator::ReusePictureBuffer(int32_t picture_buffer_id) {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  TRACE_EVENT1("Video Decoder", "OVDA::ReusePictureBuffer",
               "Picture id", picture_buffer_id);
  std::unique_ptr<PictureSyncObject> egl_sync_obj(
      new PictureSyncObject(egl_display_));

  // Start checking sync status periodically.
  CheckPictureStatus(picture_buffer_id, std::move(egl_sync_obj));
}

void OmxVideoDecodeAccelerator::CheckPictureStatus(
    int32_t picture_buffer_id,
    std::unique_ptr<PictureSyncObject> egl_sync_obj
    ) {
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  // It's possible for this task to never run if the message loop is
  // stopped. In that case we may never call QueuePictureBuffer().
  // This is fine though, because all pictures, irrespective of their state,
  // are in pictures_ map and that's what will be used to do the clean up.
  if (!egl_sync_obj->IsSynced()) {
    child_task_runner_->PostDelayedTask(FROM_HERE, base::Bind(
        &OmxVideoDecodeAccelerator::CheckPictureStatus, weak_this_,
        picture_buffer_id, base::Passed(&egl_sync_obj)),
        base::TimeDelta::FromMilliseconds(kSyncPollDelayMs));
    return;
  }
  // Synced successfully. Queue the buffer for reuse.
  QueuePictureBuffer(picture_buffer_id);
}

void OmxVideoDecodeAccelerator::QueuePictureBuffer(int32_t picture_buffer_id) {
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  // During port-flushing, do not call OMX FillThisBuffer.
  if (current_state_change_ == RESETTING) {
    queued_picture_buffer_ids_.push_back(picture_buffer_id);
    return;
  }

  // We might have started destroying while waiting for the picture. It's safe
  // to drop it here, because we will free all the pictures regardless of their
  // state using the pictures_ map.
  if (!CanFillBuffer())
    return;

  OutputPictureById::iterator it = pictures_.find(picture_buffer_id);
  RETURN_ON_FAILURE(it != pictures_.end(),
                    "Missing picture buffer id: " << picture_buffer_id,
                    INVALID_ARGUMENT,);
  OutputPicture& output_picture = it->second;

  ++output_buffers_at_component_;
  OMX_ERRORTYPE result =
      OMX_FillThisBuffer(component_handle_, output_picture.omx_buffer_header);
  RETURN_ON_OMX_FAILURE(result, "OMX_FillThisBuffer() failed",
                        PLATFORM_FAILURE,);
}

void OmxVideoDecodeAccelerator::Flush() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(current_state_change_, NO_TRANSITION);
  DCHECK_EQ(client_state_, OMX_StateExecuting);
  current_state_change_ = FLUSHING;

  Decode(media::BitstreamBuffer(-1, base::SharedMemoryHandle(), 0));
}

void OmxVideoDecodeAccelerator::OnReachedEOSInFlushing() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(client_state_, OMX_StateExecuting);

  current_state_change_ = NO_TRANSITION;
  if (client_)
    client_->NotifyFlushDone();
}

void OmxVideoDecodeAccelerator::FlushIOPorts() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  // Flush input port first.
  if (!SendCommandToPort(OMX_CommandFlush, input_port_))
    return;
}

void OmxVideoDecodeAccelerator::InputPortFlushDone() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(input_buffers_at_component_, 0);
  if (!SendCommandToPort(OMX_CommandFlush, output_port_))
    return;
}

void OmxVideoDecodeAccelerator::OutputPortFlushDone() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(output_buffers_at_component_, 0);
  BeginTransitionToState(OMX_StateExecuting);
}

void OmxVideoDecodeAccelerator::Reset() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(current_state_change_, NO_TRANSITION);
  DCHECK_EQ(client_state_, OMX_StateExecuting);
  current_state_change_ = RESETTING;
  BeginTransitionToState(OMX_StatePause);
}

void OmxVideoDecodeAccelerator::Destroy() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  std::unique_ptr<OmxVideoDecodeAccelerator> deleter(this);
  client_ptr_factory_->InvalidateWeakPtrs();

  if (current_state_change_ == ERRORING ||
      current_state_change_ == DESTROYING) {
    return;
  }

  DCHECK(current_state_change_ == NO_TRANSITION ||
         current_state_change_ == FLUSHING ||
         current_state_change_ == RESETTING) << current_state_change_;

  // If we were never initializeed there's no teardown to do.
  if (client_state_ == OMX_StateMax)
    return;
  // If we can already call OMX_FreeHandle, simply do so.
  if (client_state_ == OMX_StateInvalid || client_state_ == OMX_StateLoaded) {
    ShutdownComponent();
    return;
  }
  DCHECK(client_state_ == OMX_StateExecuting ||
         client_state_ == OMX_StateIdle ||
         client_state_ == OMX_StatePause);
  current_state_change_ = DESTROYING;
  BeginTransitionToState(OMX_StateIdle);
  BusyLoopInDestroying(std::move(deleter));
}

void OmxVideoDecodeAccelerator::BeginTransitionToState(
    OMX_STATETYPE new_state) {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  if (new_state != OMX_StateInvalid)
    DCHECK_NE(current_state_change_, NO_TRANSITION);
  if (current_state_change_ == ERRORING)
    return;
  OMX_ERRORTYPE result = OMX_SendCommand(
      component_handle_, OMX_CommandStateSet, new_state, 0);
  RETURN_ON_OMX_FAILURE(result, "SendCommand(OMX_CommandStateSet) failed",
                        PLATFORM_FAILURE,);
}

void OmxVideoDecodeAccelerator::OnReachedIdleInInitializing() {
  DCHECK_EQ(client_state_, OMX_StateLoaded);
  client_state_ = OMX_StateIdle;
  BeginTransitionToState(OMX_StateExecuting);
}

void OmxVideoDecodeAccelerator::OnReachedExecutingInInitializing() {
  DCHECK_EQ(client_state_, OMX_StateIdle);
  client_state_ = OMX_StateExecuting;
  current_state_change_ = NO_TRANSITION;

  // Request filling of our fake buffers to trigger decode processing.  In
  // reality as soon as any data is decoded these will get dismissed due to
  // dimension mismatch.
  for (std::set<OMX_BUFFERHEADERTYPE*>::iterator it =
           fake_output_buffers_.begin();
       it != fake_output_buffers_.end(); ++it) {
    OMX_BUFFERHEADERTYPE* buffer = *it;
    OMX_ERRORTYPE result = OMX_FillThisBuffer(component_handle_, buffer);
    RETURN_ON_OMX_FAILURE(result, "OMX_FillThisBuffer()", PLATFORM_FAILURE,);
    ++output_buffers_at_component_;
  }

  if (client_)
    client_->NotifyInitializationComplete(true);
}

void OmxVideoDecodeAccelerator::OnReachedPauseInResetting() {
  DCHECK_EQ(client_state_, OMX_StateExecuting);
  client_state_ = OMX_StatePause;
  FlushIOPorts();
}

void OmxVideoDecodeAccelerator::DecodeQueuedBitstreamBuffers() {
  BitstreamBufferList buffers;
  buffers.swap(queued_bitstream_buffers_);
  if (current_state_change_ == DESTROYING ||
      current_state_change_ == ERRORING) {
    return;
  }
  for (size_t i = 0; i < buffers.size(); ++i)
    Decode(buffers[i]);
}

void OmxVideoDecodeAccelerator::OnReachedExecutingInResetting() {
  DCHECK_EQ(client_state_, OMX_StatePause);
  client_state_ = OMX_StateExecuting;
  current_state_change_ = NO_TRANSITION;
  if (!client_)
    return;

  // Drain queues of input & output buffers held during the reset.
  DecodeQueuedBitstreamBuffers();
  for (size_t i = 0; i < queued_picture_buffer_ids_.size(); ++i)
    ReusePictureBuffer(queued_picture_buffer_ids_[i]);
  queued_picture_buffer_ids_.clear();

  client_->NotifyResetDone();
}

// Alert: HORROR ahead!  OMX shutdown is an asynchronous dance but our clients
// enjoy the fire-and-forget nature of a synchronous Destroy() call that
// ensures no further callbacks are made.  Since the interface between OMX
// callbacks and this class is a MessageLoop, we need to ensure the loop
// outlives the shutdown dance, even during process shutdown.  We do this by
// repeatedly enqueuing a no-op task until shutdown is complete, since
// MessageLoop's shutdown drains pending tasks.
// TODO: Yes indeed.  Review this.
void OmxVideoDecodeAccelerator::BusyLoopInDestroying(
    std::unique_ptr<OmxVideoDecodeAccelerator> self) {
  if (!component_handle_) return;
  // Can't use PostDelayedTask here because MessageLoop doesn't drain delayed
  // tasks.  Instead we sleep for 5ms.  Really.
  base::PlatformThread::Sleep(base::TimeDelta::FromMilliseconds(5));
  self->child_task_runner_->PostTask(FROM_HERE, base::Bind(
      &OmxVideoDecodeAccelerator::BusyLoopInDestroying,
      base::Unretained(this), base::Passed(&self)));
}

void OmxVideoDecodeAccelerator::OnReachedIdleInDestroying() {
  DCHECK(client_state_ == OMX_StateExecuting ||
         client_state_ == OMX_StateIdle ||
         client_state_ == OMX_StatePause);
  client_state_ = OMX_StateIdle;

  // Note that during the Executing -> Idle transition, the OMX spec guarantees
  // buffers have been returned to the client, so we don't need to do an
  // explicit FlushIOPorts().

  BeginTransitionToState(OMX_StateLoaded);

  FreeOMXBuffers();
}

void OmxVideoDecodeAccelerator::OnReachedLoadedInDestroying() {
  DCHECK_EQ(client_state_, OMX_StateIdle);
  client_state_ = OMX_StateLoaded;
  current_state_change_ = NO_TRANSITION;
  ShutdownComponent();
}

void OmxVideoDecodeAccelerator::OnReachedInvalidInErroring() {
  client_state_ = OMX_StateInvalid;
  FreeOMXBuffers();
  ShutdownComponent();
}

void OmxVideoDecodeAccelerator::ShutdownComponent() {
  OMX_ERRORTYPE result = OMX_FreeHandle(component_handle_);
  if (result != OMX_ErrorNone)
    DLOG(ERROR) << "OMX_FreeHandle() error. Error code: " << result;
  client_state_ = OMX_StateMax;
  OMX_Deinit();
  // Allow BusyLoopInDestroying to exit and delete |this|.
  component_handle_ = NULL;
}

void OmxVideoDecodeAccelerator::StopOnError(
    media::VideoDecodeAccelerator::Error error) {
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  if (current_state_change_ == ERRORING)
    return;

  if (client_ && init_begun_)
    client_->NotifyError(error);
  client_ptr_factory_->InvalidateWeakPtrs();

  if (client_state_ == OMX_StateInvalid || client_state_ == OMX_StateMax)
      return;

  BeginTransitionToState(OMX_StateInvalid);
  current_state_change_ = ERRORING;
}

bool OmxVideoDecodeAccelerator::AllocateInputBuffers() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  for (int i = 0; i < input_buffer_count_; ++i) {
    OMX_BUFFERHEADERTYPE* buffer;
    OMX_ERRORTYPE result =
        OMX_UseBuffer(component_handle_, &buffer, input_port_,
                      NULL, /* pAppPrivate gets set in Decode(). */
                      0, reinterpret_cast<OMX_U8*>(0x1));
    RETURN_ON_OMX_FAILURE(result, "OMX_UseBuffer() Input buffer error",
                          PLATFORM_FAILURE, false);
    buffer->nInputPortIndex = input_port_;
    buffer->nOffset = 0;
    buffer->nFlags = 0;
    free_input_buffers_.push(buffer);
  }
  return true;
}

bool OmxVideoDecodeAccelerator::AllocateFakeOutputBuffers() {
  // Fill the component with fake output buffers.
  for (unsigned int i = 0; i < kNumPictureBuffers; ++i) {
    OMX_BUFFERHEADERTYPE* buffer;
    OMX_ERRORTYPE result;
    result = OMX_AllocateBuffer(component_handle_, &buffer, output_port_,
                                NULL, 0);
    RETURN_ON_OMX_FAILURE(result, "OMX_AllocateBuffer failed",
                          PLATFORM_FAILURE, false);
    buffer->pAppPrivate = NULL;
    buffer->nTimeStamp = -1;
    buffer->nOutputPortIndex = output_port_;
    CHECK(fake_output_buffers_.insert(buffer).second);
  }
  return true;
}

bool OmxVideoDecodeAccelerator::AllocateOutputBuffers() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  DCHECK(!pictures_.empty());
  for (OutputPictureById::iterator it = pictures_.begin();
       it != pictures_.end(); ++it) {
    media::PictureBuffer& picture_buffer = it->second.picture_buffer;
    OMX_BUFFERHEADERTYPE** omx_buffer = &it->second.omx_buffer_header;
    DCHECK(!*omx_buffer);
    OMX_ERRORTYPE result = OMX_UseBuffer(
        component_handle_, omx_buffer, output_port_, NULL, 0,
        reinterpret_cast<OMX_U8*>(&picture_buffer)); //TODO: Is this buffer private data?

    RETURN_ON_OMX_FAILURE(result, "OMX_UseBuffer", PLATFORM_FAILURE, false);

    // Here we set a garbage bitstream buffer id, and then overwrite it before
    // passing to PictureReady. //TODO: why?
    int garbage_bitstream_buffer_id = -1;
    media::Picture* picture =
        new media::Picture(picture_buffer.id(), garbage_bitstream_buffer_id,
            gfx::Rect(), gfx::ColorSpace(), false); //TODO: Set up correct visible_rect size and color
    (*omx_buffer)->pAppPrivate = picture;
  }
  return true;
}

void OmxVideoDecodeAccelerator::FreeOMXBuffers() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  bool failure_seen = false;
  while (!free_input_buffers_.empty()) {
    OMX_BUFFERHEADERTYPE* omx_buffer = free_input_buffers_.front();
    free_input_buffers_.pop();
    OMX_ERRORTYPE result =
        OMX_FreeBuffer(component_handle_, input_port_, omx_buffer);
    if (result != OMX_ErrorNone) {
      DLOG(ERROR) << "OMX_FreeBuffer failed: 0x" << std::hex << result;
      failure_seen = true;
    }
  }
  for (OutputPictureById::iterator it = pictures_.begin();
       it != pictures_.end(); ++it) {
    OMX_BUFFERHEADERTYPE* omx_buffer = it->second.omx_buffer_header;
    DCHECK(omx_buffer);
    delete reinterpret_cast<media::Picture*>(omx_buffer->pAppPrivate);
    OMX_ERRORTYPE result =
        OMX_FreeBuffer(component_handle_, output_port_, omx_buffer);
    if (result != OMX_ErrorNone) {
      DLOG(ERROR) << "OMX_FreeBuffer failed: 0x" << std::hex << result;
      failure_seen = true;
    }
    if (client_)
      client_->DismissPictureBuffer(it->first);
  }
  pictures_.clear();

  // Delete pending fake_output_buffers_ //TODO: still not liking these
  for (std::set<OMX_BUFFERHEADERTYPE*>::iterator it =
           fake_output_buffers_.begin();
       it != fake_output_buffers_.end(); ++it) {
    OMX_BUFFERHEADERTYPE* buffer = *it;
    OMX_ERRORTYPE result =
        OMX_FreeBuffer(component_handle_, output_port_, buffer);
    if (result != OMX_ErrorNone) {
      DLOG(ERROR) << "OMX_FreeBuffer failed: 0x" << std::hex << result;
      failure_seen = true;
    }
  }
  fake_output_buffers_.clear();

  // Dequeue pending queued_picture_buffer_ids_
  if (client_) {
    for (size_t i = 0; i < queued_picture_buffer_ids_.size(); ++i)
      client_->DismissPictureBuffer(queued_picture_buffer_ids_[i]);
  }
  queued_picture_buffer_ids_.clear();

  RETURN_ON_FAILURE(!failure_seen, "OMX_FreeBuffer", PLATFORM_FAILURE,);
}

void OmxVideoDecodeAccelerator::OnOutputPortDisabled() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  OMX_PARAM_PORTDEFINITIONTYPE port_format;
  InitParam(&port_format);
  port_format.nPortIndex = output_port_;
  OMX_ERRORTYPE result = OMX_GetParameter(
      component_handle_, OMX_IndexParamPortDefinition, &port_format);
  RETURN_ON_OMX_FAILURE(result, "OMX_GetParameter", PLATFORM_FAILURE,);
  DCHECK_LE(port_format.nBufferCountMin, kNumPictureBuffers);

  // TODO(fischman): to support mid-stream resize, need to free/dismiss any
  // |pictures_| we already have.  Make sure that the shutdown-path agrees with
  // this (there's already freeing logic there, which should not be duplicated).

  // Request picture buffers to be handed to the component.
  // ProvidePictureBuffers() will trigger AssignPictureBuffers, which ultimately
  // assigns the textures to the component and re-enables the port.
  const OMX_VIDEO_PORTDEFINITIONTYPE& vformat = port_format.format.video;
  last_requested_picture_buffer_dimensions_.SetSize(vformat.nFrameWidth,
                                                    vformat.nFrameHeight);
  if (client_) {
    client_->ProvidePictureBuffers(
        kNumPictureBuffers,
        VideoPixelFormat(), // TODO: Set up and check these formats
        1,
        gfx::Size(vformat.nFrameWidth, vformat.nFrameHeight),
        GL_TEXTURE_2D);
  }
}

void OmxVideoDecodeAccelerator::OnOutputPortEnabled() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  if (current_state_change_ == RESETTING) {
    for (OutputPictureById::iterator it = pictures_.begin();
         it != pictures_.end(); ++it) {
      queued_picture_buffer_ids_.push_back(it->first);
    }
    return;
  }

  if (!CanFillBuffer()) {
    StopOnError(ILLEGAL_STATE);
    return;
  }

  // Provide output buffers to decoder.
  for (OutputPictureById::iterator it = pictures_.begin();
       it != pictures_.end(); ++it) {
    OMX_BUFFERHEADERTYPE* omx_buffer = it->second.omx_buffer_header;
    DCHECK(omx_buffer);
    // Clear EOS flag.
    omx_buffer->nFlags &= ~OMX_BUFFERFLAG_EOS;
    omx_buffer->nOutputPortIndex = output_port_;
    ++output_buffers_at_component_;
    OMX_ERRORTYPE result = OMX_FillThisBuffer(component_handle_, omx_buffer);
    RETURN_ON_OMX_FAILURE(result, "OMX_FillThisBuffer() failed",
                          PLATFORM_FAILURE,);
  }
}

void OmxVideoDecodeAccelerator::FillBufferDoneTask(
    OMX_BUFFERHEADERTYPE* buffer) {

  media::Picture* picture =
      reinterpret_cast<media::Picture*>(buffer->pAppPrivate);
  int picture_buffer_id = picture ? picture->picture_buffer_id() : -1;
  TRACE_EVENT2("Video Decoder", "OVDA::FillBufferDoneTask",
               "Buffer id", buffer->nTimeStamp,
               "Picture id", picture_buffer_id);
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK_GT(output_buffers_at_component_, 0);
  --output_buffers_at_component_;

  // If we are destroying and then get a fillbuffer callback, calling into any
  // openmax function will put us in error mode, so bail now. In the RESETTING
  // case we still need to enqueue the picture ids but have to avoid giving
  // them to the client (this is handled below).
  if (current_state_change_ == DESTROYING ||
      current_state_change_ == ERRORING)
    return;

  if (fake_output_buffers_.size() && fake_output_buffers_.count(buffer)) {
    size_t erased = fake_output_buffers_.erase(buffer);
    DCHECK_EQ(erased, 1U);
    OMX_ERRORTYPE result =
        OMX_FreeBuffer(component_handle_, output_port_, buffer);
    RETURN_ON_OMX_FAILURE(result, "OMX_FreeBuffer failed", PLATFORM_FAILURE,);
    return;
  }
  DCHECK(!fake_output_buffers_.size());

  // When the EOS picture is delivered back to us, notify the client and reuse
  // the underlying picturebuffer.
  if (buffer->nFlags & OMX_BUFFERFLAG_EOS) {
    buffer->nFlags &= ~OMX_BUFFERFLAG_EOS;
    OnReachedEOSInFlushing();
    ReusePictureBuffer(picture_buffer_id);
    return;
  }

  // During the transition from Executing to Idle, and during port-flushing, all
  // pictures are sent back through here.  Avoid giving them to the client.
  if (current_state_change_ == RESETTING) {
    queued_picture_buffer_ids_.push_back(picture_buffer_id);
    return;
  }

  DCHECK(picture);
  // See Decode() for an explanation of this abuse of nTimeStamp.
  picture->set_bitstream_buffer_id(buffer->nTimeStamp);
  if (client_)
    client_->PictureReady(*picture);
}

void OmxVideoDecodeAccelerator::EmptyBufferDoneTask(
    OMX_BUFFERHEADERTYPE* buffer) {
  TRACE_EVENT1("Video Decoder", "OVDA::EmptyBufferDoneTask",
               "Buffer id", buffer->nTimeStamp);
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK_GT(input_buffers_at_component_, 0);
  free_input_buffers_.push(buffer);
  input_buffers_at_component_--;
  if (buffer->nFlags & OMX_BUFFERFLAG_EOS)
    return;

  // Retrieve the corresponding BitstreamBuffer's id and notify the client of
  // its completion.
  SharedMemoryAndId* input_buffer_details =
      reinterpret_cast<SharedMemoryAndId*>(buffer->pAppPrivate);
  DCHECK(input_buffer_details);
  buffer->pAppPrivate = NULL;
  if (client_)
    client_->NotifyEndOfBitstreamBuffer(input_buffer_details->second);
  delete input_buffer_details;

  DecodeQueuedBitstreamBuffers();
}

void OmxVideoDecodeAccelerator::DispatchStateReached(OMX_STATETYPE reached) {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  switch (current_state_change_) {
    case INITIALIZING:
      switch (reached) {
        case OMX_StateIdle:
          OnReachedIdleInInitializing();
          return;
        case OMX_StateExecuting:
          OnReachedExecutingInInitializing();
          return;
        default:
          NOTREACHED() << "Unexpected state in INITIALIZING: " << reached;
          return;
      }
    case RESETTING:
      switch (reached) {
        case OMX_StatePause:
          OnReachedPauseInResetting();
          return;
        case OMX_StateExecuting:
          OnReachedExecutingInResetting();
          return;
        default:
          NOTREACHED() << "Unexpected state in RESETTING: " << reached;
          return;
      }
    case DESTROYING:
      switch (reached) {
        case OMX_StatePause:
        case OMX_StateExecuting:
          // Because Destroy() can interrupt an in-progress Reset(),
          // we might arrive at these states after current_state_change_ was
          // overwritten with DESTROYING.  That's fine though - we already have
          // the state transition for Destroy() queued up at the component, so
          // we treat this as a no-op.
          return;
        case OMX_StateIdle:
          OnReachedIdleInDestroying();
          return;
        case OMX_StateLoaded:
          OnReachedLoadedInDestroying();
          return;
        default:
          NOTREACHED() << "Unexpected state in DESTROYING: " << reached;
          return;
      }
    case ERRORING:
      switch (reached) {
        case OMX_StateInvalid:
          OnReachedInvalidInErroring();
          return;
        default:
          NOTREACHED() << "Unexpected state in ERRORING: " << reached;
          return;
      }
    default:
      NOTREACHED() << "Unexpected state in " << current_state_change_
                   << ": " << reached;
  }
}

void OmxVideoDecodeAccelerator::EventHandlerCompleteTask(OMX_EVENTTYPE event,
                                                         OMX_U32 data1,
                                                         OMX_U32 data2) {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  switch (event) {
    case OMX_EventCmdComplete:
      switch (data1) {
        case OMX_CommandPortDisable:
          DCHECK_EQ(data2, output_port_);
          OnOutputPortDisabled();
          return;
        case OMX_CommandPortEnable:
          DCHECK_EQ(data2, output_port_);
          OnOutputPortEnabled();
          return;
        case OMX_CommandStateSet:
          DispatchStateReached(static_cast<OMX_STATETYPE>(data2));
          return;
        case OMX_CommandFlush:
          if (current_state_change_ == DESTROYING ||
              current_state_change_ == ERRORING) {
            return;
          }
          DCHECK_EQ(current_state_change_, RESETTING);
          if (data2 == input_port_)
            InputPortFlushDone();
          else if (data2 == output_port_)
            OutputPortFlushDone();
          else
            NOTREACHED() << "Unexpected port flushed: " << data2;
          return;
        default:
          RETURN_ON_FAILURE(false, "Unknown command completed: " << data1,
                            PLATFORM_FAILURE,);
      }
      return;
    case OMX_EventError:
      if (current_state_change_ != DESTROYING &&
          current_state_change_ != ERRORING) {
        RETURN_ON_FAILURE(false, "EventError: 0x" << std::hex << data1,
                          PLATFORM_FAILURE,);
      }
      return;
    case OMX_EventPortSettingsChanged:
      if (data1 == output_port_ &&
          data2 == OMX_IndexParamPortDefinition) {
        // This event is only used for output resize; kick off handling that by
        // pausing the output port.
        SendCommandToPort(OMX_CommandPortDisable, output_port_);
      } else if (data1 == output_port_ &&
                 data2 == OMX_IndexConfigCommonOutputCrop) {
        // TODO(vjain): Handle video crop rect.
      } else if (data1 == output_port_ &&
                 data2 == OMX_IndexConfigCommonScale) {
        // TODO(ashokm@nvidia.com): Handle video SAR change.
      } else {
        RETURN_ON_FAILURE(false,
                          "Unexpected EventPortSettingsChanged: "
                          << data1 << ", " << data2,
                          PLATFORM_FAILURE,);
      }
      return;
    case OMX_EventBufferFlag:
      if (data1 == output_port_) {
        // In case of Destroy() interrupting Flush().
        if (current_state_change_ == DESTROYING)
          return;
        DCHECK_EQ(current_state_change_, FLUSHING);
        // Do nothing; rely on the EOS picture delivery to notify the client.
      } else {
        RETURN_ON_FAILURE(false,
                          "Unexpected OMX_EventBufferFlag: "
                          << data1 << ", " << data2,
                          PLATFORM_FAILURE,);
      }
      return;
    default:
      RETURN_ON_FAILURE(false, "Unexpected unhandled event: " << event,
                        PLATFORM_FAILURE,);
  }
}

// static
void OmxVideoDecodeAccelerator::PreSandboxInitialization() {
  DCHECK(!pre_sandbox_init_done_);
  pre_sandbox_init_done_ = true;
}

// static
bool OmxVideoDecodeAccelerator::PostSandboxInitialization() {
  if (!pre_sandbox_init_done_)
    return false;

  StubPathMap paths;
  paths[kModuleOmx].push_back(kOMXLib);

  return InitializeStubs(paths);
}

// static
OMX_ERRORTYPE OmxVideoDecodeAccelerator::EventHandler(OMX_HANDLETYPE component,
                                                      OMX_PTR priv_data,
                                                      OMX_EVENTTYPE event,
                                                      OMX_U32 data1,
                                                      OMX_U32 data2,
                                                      OMX_PTR event_data) {
  // Called on the OMX thread.
  OmxVideoDecodeAccelerator* decoder =
      static_cast<OmxVideoDecodeAccelerator*>(priv_data);
  DCHECK_EQ(component, decoder->component_handle_);
  decoder->child_task_runner_->PostTask(FROM_HERE, base::Bind(
      &OmxVideoDecodeAccelerator::EventHandlerCompleteTask,
      decoder->weak_this(), event, data1, data2));
  return OMX_ErrorNone;
}

// static
OMX_ERRORTYPE OmxVideoDecodeAccelerator::EmptyBufferCallback(
    OMX_HANDLETYPE component,
    OMX_PTR priv_data,
    OMX_BUFFERHEADERTYPE* buffer) {
  TRACE_EVENT1("Video Decoder", "OVDA::EmptyBufferCallback",
               "Buffer id", buffer->nTimeStamp);
  // Called on the OMX thread.
  OmxVideoDecodeAccelerator* decoder =
      static_cast<OmxVideoDecodeAccelerator*>(priv_data);
  DCHECK_EQ(component, decoder->component_handle_);
  decoder->child_task_runner_->PostTask(FROM_HERE, base::Bind(
      &OmxVideoDecodeAccelerator::EmptyBufferDoneTask, decoder->weak_this(),
      buffer));
  return OMX_ErrorNone;
}

// static
OMX_ERRORTYPE OmxVideoDecodeAccelerator::FillBufferCallback(
    OMX_HANDLETYPE component,
    OMX_PTR priv_data,
    OMX_BUFFERHEADERTYPE* buffer) {
  media::Picture* picture =
      reinterpret_cast<media::Picture*>(buffer->pAppPrivate);
  int picture_buffer_id = picture ? picture->picture_buffer_id() : -1;
  TRACE_EVENT2("Video Decoder", "OVDA::FillBufferCallback",
               "Buffer id", buffer->nTimeStamp,
               "Picture id", picture_buffer_id);
  // Called on the OMX thread.
  OmxVideoDecodeAccelerator* decoder =
      static_cast<OmxVideoDecodeAccelerator*>(priv_data);
  DCHECK_EQ(component, decoder->component_handle_);
  decoder->child_task_runner_->PostTask(FROM_HERE, base::Bind(
      &OmxVideoDecodeAccelerator::FillBufferDoneTask, decoder->weak_this(),
      buffer));
  return OMX_ErrorNone;
}

bool OmxVideoDecodeAccelerator::CanFillBuffer() {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  const CurrentStateChange csc = current_state_change_;
  const OMX_STATETYPE cs = client_state_;
  return (csc != DESTROYING && csc != ERRORING && csc != RESETTING) &&
      (cs == OMX_StateIdle || cs == OMX_StateExecuting || cs == OMX_StatePause);
}

bool OmxVideoDecodeAccelerator::SendCommandToPort(
    OMX_COMMANDTYPE cmd, int port_index) {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  OMX_ERRORTYPE result = OMX_SendCommand(component_handle_,
                                         cmd, port_index, 0);
  RETURN_ON_OMX_FAILURE(result, "SendCommand() failed" << cmd,
                        PLATFORM_FAILURE, false);
  return true;
}

}  // namespace content
