// MIT License
//
// Copyright(c) 2022 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "factories.h"
#include "interfaces.h"
#include "layer.h"
#include "log.h"

namespace {

    using namespace toolkit;
    using namespace toolkit::config;
    using namespace toolkit::input;
    using namespace toolkit::utilities;
    using namespace toolkit::log;


    using namespace xr::math;

    class EyeTrackerBase : public IEyeTracker {
      public:
        EyeTrackerBase(OpenXrApi& openXR, std::shared_ptr<IConfigManager> configManager)
            : m_openXR(openXR), m_configManager(configManager) {
        }

        ~EyeTrackerBase() override {
            endSession();
        }

        void beginSession(XrSession session) override {
            m_session = session;

            // Create a reference space.
            {
                XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr};
                referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                referenceSpaceCreateInfo.poseInReferenceSpace = Pose::Identity();
                CHECK_XRCMD(m_openXR.xrCreateReferenceSpace(session, &referenceSpaceCreateInfo, &m_viewSpace));
            }
        }

        void endSession() override {
            if (m_eyeTrackerActionSet != XR_NULL_HANDLE) {
                m_openXR.xrDestroyActionSet(m_eyeTrackerActionSet);
                m_eyeTrackerActionSet = XR_NULL_HANDLE;
            }
            if (m_viewSpace != XR_NULL_HANDLE) {
                m_openXR.xrDestroySpace(m_viewSpace);
                m_viewSpace = XR_NULL_HANDLE;
            }

            m_session = XR_NULL_HANDLE;
        }

        void beginFrame(XrTime frameTime) override {
            m_frameTime = frameTime;
            m_valid = false;
        }

        void endFrame() override {
        }

        void update() {
            m_projectionDistance = m_configManager->getValue(SettingEyeProjectionDistance) / 100.f;
        }

        XrActionSet getActionSet() const override {
            return m_eyeTrackerActionSet;
        }

        virtual bool getEyeGaze(XrVector3f& projectedPoint) const = 0;

        bool getProjectedGaze(XrVector2f gaze[ViewCount]) const {
            assert(m_session != XR_NULL_HANDLE);

            if (!m_frameTime) {
                return false;
            }

            if (!m_valid) {
                // We need the FOVs so we can create a projection matrix.
                XrView eyeInViewSpace[2] = {{XR_TYPE_VIEW, nullptr}, {XR_TYPE_VIEW, nullptr}};
                {
                    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO, nullptr};
                    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    locateInfo.space = m_viewSpace;
                    locateInfo.displayTime = m_frameTime;

                    XrViewState state{XR_TYPE_VIEW_STATE, nullptr};
                    uint32_t viewCountOutput;
                    CHECK_HRCMD(
                        m_openXR.xrLocateViews(m_session, &locateInfo, &state, 2, &viewCountOutput, eyeInViewSpace));

                    if (!Pose::IsPoseValid(state.viewStateFlags)) {
                        return false;
                    }
                }

                XrVector3f projectedPoint{};
                if (!getEyeGaze(projectedPoint)) {
                    return false;
                }

                m_eyeGazeState.gazeRay = projectedPoint;

                // Project the pose onto the screen.
                m_valid = GetProjectedGaze(eyeInViewSpace, projectedPoint, m_gaze);

                if (m_valid) {
                    m_eyeGazeState.leftPoint.x = m_gaze[0].x;
                    m_eyeGazeState.leftPoint.y = m_gaze[0].y;
                    m_eyeGazeState.rightPoint.x = m_gaze[1].x;
                    m_eyeGazeState.rightPoint.y = m_gaze[1].y;
                }
            }

            for (uint32_t eye = 0; eye < ViewCount; eye++) {
                gaze[eye] = m_gaze[eye];
            }

            return true;
        }

        const EyeGazeState& getEyeGazeState() const override {
            return m_eyeGazeState;
        }

      protected:
        OpenXrApi& m_openXR;
        const std::shared_ptr<IConfigManager> m_configManager;
        float m_projectionDistance{2.f};

        XrSession m_session{XR_NULL_HANDLE};
        XrSpace m_viewSpace{XR_NULL_HANDLE};
        XrTime m_frameTime{0};

        XrActionSet m_eyeTrackerActionSet{XR_NULL_HANDLE};

        mutable XrVector2f m_gaze[ViewCount];
        mutable bool m_valid{false};
        mutable EyeGazeState m_eyeGazeState{};
    };

    class OpenXrEyeTracker : public EyeTrackerBase {
      public:
        OpenXrEyeTracker(OpenXrApi& openXR, std::shared_ptr<IConfigManager> configManager)
            : EyeTrackerBase(openXR, configManager) {
        }

        ~OpenXrEyeTracker() override {
        }

        void beginSession(XrSession session) override {
            EyeTrackerBase::beginSession(session);

            m_debugWithController = m_configManager->getValue(SettingEyeDebugWithController);

            // Create the resources for the eye tracker space.
            {
                XrActionSetCreateInfo actionSetCreateInfo{XR_TYPE_ACTION_SET_CREATE_INFO, nullptr};
                strcpy_s(actionSetCreateInfo.actionSetName, "eye_tracker");
                strcpy_s(actionSetCreateInfo.localizedActionSetName, "Eye Tracker");
                actionSetCreateInfo.priority = 0;
                CHECK_XRCMD(
                    m_openXR.xrCreateActionSet(m_openXR.GetXrInstance(), &actionSetCreateInfo, &m_eyeTrackerActionSet));
            }
            {
                XrActionCreateInfo actionCreateInfo{XR_TYPE_ACTION_CREATE_INFO, nullptr};
                strcpy_s(actionCreateInfo.actionName, "eye_tracker");
                strcpy_s(actionCreateInfo.localizedActionName, "Eye Tracker");
                actionCreateInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
                actionCreateInfo.countSubactionPaths = 0;
                CHECK_XRCMD(m_openXR.xrCreateAction(m_eyeTrackerActionSet, &actionCreateInfo, &m_eyeGazeAction));
            }
            {
                XrActionSuggestedBinding binding;
                binding.action = m_eyeGazeAction;

                XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                                                       nullptr};

                if (!m_debugWithController) {
                    CHECK_XRCMD(m_openXR.xrStringToPath(
                        m_openXR.GetXrInstance(), "/user/eyes_ext/input/gaze_ext/pose", &binding.binding));
                    CHECK_XRCMD(m_openXR.xrStringToPath(m_openXR.GetXrInstance(),
                                                        "/interaction_profiles/ext/eye_gaze_interaction",
                                                        &suggestedBindings.interactionProfile));
                } else {
                    // We use a Left HP motion controller to simulate the eye gaze.
                    CHECK_XRCMD(m_openXR.xrStringToPath(
                        m_openXR.GetXrInstance(), "/user/hand/left/input/grip/pose", &binding.binding));
                    CHECK_XRCMD(m_openXR.xrStringToPath(m_openXR.GetXrInstance(),
                                                        "/interaction_profiles/hp/mixed_reality_controller",
                                                        &suggestedBindings.interactionProfile));
                }
                suggestedBindings.suggestedBindings = &binding;
                suggestedBindings.countSuggestedBindings = 1;
                CHECK_XRCMD(m_openXR.xrSuggestInteractionProfileBindings(m_openXR.GetXrInstance(), &suggestedBindings));
            }
            {
                XrActionSpaceCreateInfo actionSpaceCreateInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr};
                actionSpaceCreateInfo.action = m_eyeGazeAction;
                actionSpaceCreateInfo.subactionPath = XR_NULL_PATH;
                actionSpaceCreateInfo.poseInActionSpace = Pose::Identity();
                CHECK_XRCMD(m_openXR.xrCreateActionSpace(m_session, &actionSpaceCreateInfo, &m_eyeSpace));
            }
        }

        void endSession() override {
            if (m_eyeSpace != XR_NULL_HANDLE) {
                m_openXR.xrDestroySpace(m_eyeSpace);
                m_eyeSpace = XR_NULL_HANDLE;
            }
            if (m_eyeGazeAction != XR_NULL_HANDLE) {
                m_openXR.xrDestroyAction(m_eyeGazeAction);
                m_eyeGazeAction = XR_NULL_HANDLE;
            }

            EyeTrackerBase::endSession();
        }

        bool getEyeGaze(XrVector3f& projectedPoint) const override {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, nullptr};

            // Query the latest eye gaze pose.
            {
                XrActionStatePose actionStatePose{XR_TYPE_ACTION_STATE_POSE, nullptr};
                XrActionStateGetInfo getActionStateInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr};
                getActionStateInfo.action = m_eyeGazeAction;
                CHECK_XRCMD(m_openXR.xrGetActionStatePose(m_session, &getActionStateInfo, &actionStatePose));

                if (!actionStatePose.isActive) {
                    return false;
                }
            }

            CHECK_XRCMD(m_openXR.xrLocateSpace(m_eyeSpace, m_viewSpace, m_frameTime, &location));

            if (!Pose::IsPoseValid(location.locationFlags)) {
                return false;
            }

            if (m_debugWithController) {
                location.pose.position.x = location.pose.position.y = location.pose.position.z = 0.f;
            }

            // Project 2m forward along the gaze orientation. In debug-with-controller mode, the
            // position was just zeroed above (to discard the controller's translational offset),
            // but the orientation is kept, so this still yields a meaningful, non-degenerate
            // direction instead of the (0,0,0) null vector the old code produced here.
            const auto gaze = LoadXrPose(location.pose);
            const auto gazeProjectedPoint =
                DirectX::XMVector3Transform(DirectX::XMVectorSet(0, 0, 2, 1) /* 2m forward */, gaze);

            projectedPoint.x = gazeProjectedPoint.m128_f32[0];
            projectedPoint.y = gazeProjectedPoint.m128_f32[1];
            projectedPoint.z = gazeProjectedPoint.m128_f32[2];

            return true;
        }

        bool isProjectionDistanceSupported() const {
            return false;
        }

      private:
        bool m_debugWithController{false};
        XrAction m_eyeGazeAction{XR_NULL_HANDLE};
        XrSpace m_eyeSpace{XR_NULL_HANDLE};
    };

    // Meta/Oculus eye tracking via the XR_FB_eye_tracking_social extension. No third-party SDK
    // needed - this is a standard OpenXR extension, loaded dynamically like the other optional
    // extensions this layer uses (see xrCreateInstance()'s hasEyeTrackerFB detection).
    class OpenXrFBEyeTracker : public EyeTrackerBase {
      public:
        OpenXrFBEyeTracker(OpenXrApi& openXR, std::shared_ptr<IConfigManager> configManager)
            : EyeTrackerBase(openXR, configManager) {
            CHECK_XRCMD(m_openXR.xrGetInstanceProcAddr(m_openXR.GetXrInstance(),
                                                       "xrCreateEyeTrackerFB",
                                                       reinterpret_cast<PFN_xrVoidFunction*>(&m_xrCreateEyeTrackerFB)));
            CHECK_XRCMD(
                m_openXR.xrGetInstanceProcAddr(m_openXR.GetXrInstance(),
                                               "xrDestroyEyeTrackerFB",
                                               reinterpret_cast<PFN_xrVoidFunction*>(&m_xrDestroyEyeTrackerFB)));
            CHECK_XRCMD(m_openXR.xrGetInstanceProcAddr(m_openXR.GetXrInstance(),
                                                       "xrGetEyeGazesFB",
                                                       reinterpret_cast<PFN_xrVoidFunction*>(&m_xrGetEyeGazesFB)));
        }

        ~OpenXrFBEyeTracker() override {
        }

        void beginSession(XrSession session) override {
            EyeTrackerBase::beginSession(session);

            // Create the resources for the eye tracker.
            XrEyeTrackerCreateInfoFB createInfo{XR_TYPE_EYE_TRACKER_CREATE_INFO_FB};
            const XrResult result = m_xrCreateEyeTrackerFB(session, &createInfo, &m_eyeTracker);
            if (XR_FAILED(result)) {
                if (result != XR_ERROR_RUNTIME_FAILURE) {
                    CHECK_XRCMD(result);
                } else {
                    Log("xrCreateEyeTrackerFB() failed with XR_ERROR_RUNTIME_FAILURE! This is an Oculus platform "
                        "software bug, please file a report to Meta!\n");
                }
            }
        }

        void endSession() override {
            if (m_eyeTracker != XR_NULL_HANDLE) {
                m_xrDestroyEyeTrackerFB(m_eyeTracker);
                m_eyeTracker = XR_NULL_HANDLE;
            }

            EyeTrackerBase::endSession();
        }

        bool getEyeGaze(XrVector3f& projectedPoint) const override {
            if (m_eyeTracker == XR_NULL_HANDLE) {
                return false;
            }

            XrEyeGazesInfoFB eyeGazeInfo{XR_TYPE_EYE_GAZES_INFO_FB};
            eyeGazeInfo.baseSpace = m_viewSpace;
            eyeGazeInfo.time = m_frameTime;

            XrEyeGazesFB eyeGaze{XR_TYPE_EYE_GAZES_FB};

            CHECK_XRCMD(m_xrGetEyeGazesFB(m_eyeTracker, &eyeGazeInfo, &eyeGaze));

            if (!(eyeGaze.gaze[0].isValid && eyeGaze.gaze[1].isValid)) {
                return false;
            }

            if (!(eyeGaze.gaze[0].gazeConfidence > 0.5f && eyeGaze.gaze[1].gazeConfidence > 0.5f)) {
                return false;
            }

            // Average the poses from both eyes.
            const auto gaze = LoadXrPose(Pose::Slerp(eyeGaze.gaze[0].gazePose, eyeGaze.gaze[1].gazePose, 0.5f));
            const auto gazeProjectedPoint =
                DirectX::XMVector3Transform(DirectX::XMVectorSet(0, 0, m_projectionDistance, 1), gaze);

            projectedPoint.x = gazeProjectedPoint.m128_f32[0];
            projectedPoint.y = gazeProjectedPoint.m128_f32[1];
            projectedPoint.z = gazeProjectedPoint.m128_f32[2];

            return true;
        }

        bool isProjectionDistanceSupported() const {
            return true;
        }

      private:
        PFN_xrCreateEyeTrackerFB m_xrCreateEyeTrackerFB{nullptr};
        PFN_xrDestroyEyeTrackerFB m_xrDestroyEyeTrackerFB{nullptr};
        PFN_xrGetEyeGazesFB m_xrGetEyeGazesFB{nullptr};

        XrEyeTrackerFB m_eyeTracker{XR_NULL_HANDLE};
        XrSpace m_eyeSpace{XR_NULL_HANDLE};
    };

    // Pimax's aSeeVR eye tracker (Droolon add-on), via the proprietary aSeeVRClient SDK
    // (external/aSeeVRClient). Callback-driven: the SDK pushes state/eye-data/coefficient updates
    // on its own thread via aSeeVR_register_callback(), rather than us polling it.
    class PimaxEyeTracker : public EyeTrackerBase {
      public:
        PimaxEyeTracker(OpenXrApi& openXR, std::shared_ptr<IConfigManager> configManager)
            : EyeTrackerBase(openXR, configManager), m_state(std::make_unique<SharedState>().release()) {
            // Registered callbacks run on the SDK's own thread and only ever touch `m_state`
            // (see SharedState below) - never `this` - so there is nothing here for the
            // destructor to detach, and no need to keep `this` alive for their duration.
            //
            // aSeeVR_register_callback()'s second parameter is declared as a generic `void*`,
            // not a typed function-pointer typedef (per the aSeeVR UserSDK headers), and converting
            // a function pointer to void* is not an implicit standard conversion in C++. This
            // project builds with ConformanceMode (/permissive-), which rejects the non-standard
            // implicit conversion MSVC otherwise tolerates - hence the explicit casts below.
            aSeeVR_register_callback(
                aSeeVRCallbackType::state, reinterpret_cast<void*>(stateCallback), m_state);
            aSeeVR_register_callback(
                aSeeVRCallbackType::eye_data, reinterpret_cast<void*>(eyeDataCallback), m_state);
            aSeeVR_register_callback(
                aSeeVRCallbackType::coefficient, reinterpret_cast<void*>(getCoefficientCallback), m_state);
        }

        ~PimaxEyeTracker() override {
            endSession();

            // The aSeeVR SDK offers no callback-unregister API, so a callback can still be in
            // flight on the SDK's own thread (or arrive late) after this destructor runs. Since
            // callbacks only ever touch `m_state` - a self-contained, independently
            // mutex-guarded block - and never `this` or any other member of this class, it is
            // safe to destroy `this` at any time without further synchronization here.
            // `m_state` itself is intentionally never deleted (see SharedState) - that's not a
            // per-call leak, just a one-time, bounded leak per PimaxEyeTracker instance. Note
            // that this tracker is constructed once per OpenXR instance (see xrCreateInstance()
            // in layer.cpp), not once per session, so this leak cannot accumulate across the
            // many sessions an application may create against one instance.
        }

        void beginSession(XrSession session) override {
            EyeTrackerBase::beginSession(session);

            const auto status = aSeeVR_get_coefficient();
            if (status != ASEEVR_RETURN_CODE::success) {
                Log("aSeeVR_get_coefficient failed with: %d\n", status);
            }
        }

        void endSession() override {
            // Assumed idempotent: endSession() (and therefore aSeeVR_stop()) can run twice over
            // this object's lifetime if the application destroys and later recreates its
            // session, since both xrDestroySession and the destructor route through here.
            aSeeVR_stop();

            EyeTrackerBase::endSession();
        }

        bool getEyeGaze(XrVector3f& projectedPoint) const override {
            std::lock_guard<std::mutex> lock(m_state->mutex);

            if (!m_state->isDeviceReady) {
                return false;
            }

            // TODO: Use timestamp to implement a timeout.

            // The point is projected onto a screen at Z = -m_projectionDistance.
            projectedPoint.x = m_state->recommendedGaze.x - 0.5f;
            projectedPoint.y = m_state->recommendedGaze.y - 0.5f;
            projectedPoint.z = -m_projectionDistance;

            return true;
        }

        bool isProjectionDistanceSupported() const {
            return true;
        }

      private:
        // State written by the SDK's callback thread and read by getEyeGaze() on the render
        // thread, guarded by its own mutex. Allocated once via `new` (through
        // std::make_unique(...).release(), so construction failure can't leak it) and never
        // deleted: the aSeeVR SDK offers no callback-unregister API, so a callback could still
        // land on this address after the owning PimaxEyeTracker is destroyed, and freeing it
        // would turn that into a use-after-free. Deliberately holding no pointer back to the
        // owning PimaxEyeTracker keeps this independent of that object's lifetime entirely.
        struct SharedState {
            std::mutex mutex;
            bool isDeviceReady{false};
            XrVector2f recommendedGaze{0, 0};
            aSeeVRCoefficient coefficients{};
            int64_t lastTimestamp{0};
        };

        SharedState* const m_state;

        static void _7INVENSUN_CALL stateCallback(const aSeeVRState* state, void* context) {
            if (!state) {
                return;
            }

            switch (state->code) {
            case aSeeVRStateCode::api_start:
                Log("aSeeVR_start completed with: %d\n", state->error);
                break;

            case aSeeVRStateCode::api_stop:
                Log("aSeeVR_stop completed with: %d\n", state->error);
                break;

            default:
                break;
            }
        }

        static void _7INVENSUN_CALL eyeDataCallback(const aSeeVREyeData* eyeData, void* context) {
            if (!eyeData) {
                return;
            }

            int64_t timestamp = 0;
            aSeeVR_get_int64(eyeData, aSeeVREye::undefine_eye, aSeeVREyeDataItemType::timestamp, &timestamp);

            aSeeVRPoint2D point2D = {0};
            aSeeVR_get_point2d(eyeData, aSeeVREye::undefine_eye, aSeeVREyeDataItemType::gaze, &point2D);

            auto* state = reinterpret_cast<SharedState*>(context);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->recommendedGaze = {point2D.x, point2D.y};
            state->lastTimestamp = timestamp;
        }

        static void _7INVENSUN_CALL getCoefficientCallback(const aSeeVRCoefficient* data, void* context) {
            if (!data) {
                return;
            }

            auto* state = reinterpret_cast<SharedState*>(context);

            // Hold the lock only long enough to copy the coefficients in; aSeeVR_start() is
            // called below without it held. If aSeeVR_start() ever turns out to deliver another
            // SDK callback (state/eye_data/coefficient) synchronously on this same thread, that
            // callback would also need state->mutex, and calling aSeeVR_start() while already
            // holding it would deadlock.
            aSeeVRCoefficient coefficients;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->coefficients = *data;
                coefficients = state->coefficients;
            }

            const auto status = aSeeVR_start(&coefficients);

            std::lock_guard<std::mutex> lock(state->mutex);
            if (status == ASEEVR_RETURN_CODE::success) {
                state->isDeviceReady = true;
            } else {
                Log("aSeeVR_start failed with: %d\n", status);
            }
        }
    };

} // namespace

namespace toolkit::input {
    std::shared_ptr<IEyeTracker> CreateEyeTracker(toolkit::OpenXrApi& openXR,
                                                  std::shared_ptr<toolkit::config::IConfigManager> configManager) {
        return std::make_shared<OpenXrEyeTracker>(openXR, configManager);
    }

    std::shared_ptr<IEyeTracker> CreateEyeTrackerFB(toolkit::OpenXrApi& openXR,
                                                    std::shared_ptr<toolkit::config::IConfigManager> configManager) {
        return std::make_shared<OpenXrFBEyeTracker>(openXR, configManager);
    }

    std::shared_ptr<IEyeTracker> CreatePimaxEyeTracker(toolkit::OpenXrApi& openXR,
                                                       std::shared_ptr<toolkit::config::IConfigManager> configManager) {
        return std::make_shared<PimaxEyeTracker>(openXR, configManager);
    }

} // namespace toolkit::input
