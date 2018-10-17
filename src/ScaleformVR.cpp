#include "ScaleformVR.h"

#include "skse64/ScaleformValue.h"
#include "skse64/ScaleformMovie.h"
#include "skse64/Hooks_Scaleform.h"
#include "skse64/ScaleformCallbacks.h"
#include <list>
#include "openvrhooks.h"
#include <time.h>

namespace ScaleformVR
{
	bool equalGFxValue(const GFxValue& lhs, const GFxValue& rhs)
	{
		if (rhs.type == lhs.type) {
			switch (rhs.GetType()) {
			case GFxValue::kType_Null:
				return true;
			case GFxValue::kType_Bool:
				return lhs.GetBool() == rhs.GetBool();
			case GFxValue::kType_Number:
				return lhs.GetNumber() == rhs.GetNumber();
			case GFxValue::kType_String:
				return 0 == strcmp(lhs.GetString(), rhs.GetString());
			case GFxValue::kType_WideString:
				return 0 == wcscmp(lhs.GetWideString(), rhs.GetWideString());
			case GFxValue::kType_Object:
				return lhs.data.obj == rhs.data.obj;
			case GFxValue::kType_Array:
				// Not implemented =(
				return false;
			case GFxValue::kType_DisplayObject:
				return lhs.data.obj == rhs.data.obj;
			default:
				return false;
			}
		}
		return false;
	}

	struct ScaleformCallback {
		GFxMovieView *	movieView;
		GFxValue		object;
		const char *	methodName;

		bool operator==(const ScaleformCallback& rhs)
		{
			return equalGFxValue(this->object, rhs.object) && (0 == strcmp(this->methodName, rhs.methodName));
		}
	};

	typedef std::list <ScaleformCallback> ScaleformCallbackList;
	static ScaleformCallbackList g_scaleformInputHandlers;

	void DispatchControllerState(vr::ETrackedControllerRole controllerHand, vr::VRControllerState_t controllerState) {
		for (ScaleformCallbackList::iterator iter = g_scaleformInputHandlers.begin(); iter != g_scaleformInputHandlers.end(); ++iter)
		{
			GFxMovieView* movieView = (*iter).movieView;
			GFxValue* object = &(*iter).object;
			const char* methodName = (*iter).methodName;

			UInt64 highmask = 0xFFFFFFFF00000000;
			UInt64 lowmask = 0x00000000FFFFFFFF;

			GFxValue result, args[8];
			args[0].SetNumber(clock());
			args[1].SetNumber(controllerHand);
			args[2].SetNumber(controllerState.unPacketNum);

			args[3].SetNumber(controllerState.ulButtonPressed & lowmask);
			args[4].SetNumber((controllerState.ulButtonPressed & highmask)>>32);
			
			args[5].SetNumber(controllerState.ulButtonTouched & lowmask);
			args[6].SetNumber((controllerState.ulButtonTouched & highmask)>>32);

			movieView->CreateArray(&args[7]);

			for (int i=0; i < vr::k_unControllerStateAxisCount; i++) {
				GFxValue x, y;
				x.SetNumber(controllerState.rAxis[i].x);
				y.SetNumber(controllerState.rAxis[i].y);

				args[7].PushBack(&x);
				args[7].PushBack(&y);
			}

			object->Invoke(methodName, &result, args, 8);
		}
	}

	class VRInputScaleform_ShutoffButtonEventsToGame : public GFxFunctionHandler
	{
	public:
		virtual void	Invoke(Args * args)
		{
			//_MESSAGE("ShutoffButtonEventsToGame dll fn called!");

			ASSERT(args->numArgs >= 1);

			bool shutoff = args->args[0].GetBool();

			setControllerStateUpdateShutoff(shutoff);

			//_MESSAGE("ShutoffButtonEventsToGame: %d", shutoff);
		}
	};

	class VRInputScaleform_RegisterInputHandler : public GFxFunctionHandler
	{
	public:
		virtual void	Invoke(Args * args)
		{
			//_MESSAGE("RegisterInputHandler dll fn called!");

			ASSERT(args->numArgs == 2);
			ASSERT(args->args[0].GetType() == GFxValue::kType_DisplayObject);
			ASSERT(args->args[1].GetType() == GFxValue::kType_String);

			ScaleformCallback callback;
			callback.movieView = args->movie;
			callback.object = args->args[0];
			callback.methodName = args->args[1].GetString();

			// If the same callback has already been registered, do not add it again.
			// This will cause the callback to triggered twice when input events are sent.
			for (ScaleformCallbackList::iterator iter = g_scaleformInputHandlers.begin(); iter != g_scaleformInputHandlers.end(); ++iter)
			{
				if (*iter == callback) {
					//_MESSAGE("Duplicate callback found, aborting register...");
					return;
				}
			}

			//_MESSAGE("Registering: %s", callback.methodName);
			g_scaleformInputHandlers.push_back(callback);
		}
	};

	class VRInputScaleform_UnregisterInputHandler : public GFxFunctionHandler
	{
	public:
		virtual void	Invoke(Args * args)
		{
			//_MESSAGE("VRInputScaleform_UnregisterInputHandler dll fn called!");
			ASSERT(args->numArgs == 2);
			ASSERT(args->args[0].GetType() == GFxValue::kType_DisplayObject);
			ASSERT(args->args[1].GetType() == GFxValue::kType_String);

			ScaleformCallback callback;
			callback.object = args->args[0];
			callback.methodName = args->args[1].GetString();

			g_scaleformInputHandlers.remove(callback);
			//_MESSAGE("Unregistering: %s | remaining: %d", callback.methodName, g_scaleformInputHandlers.size());
		}
	};

	static float lerp(float start, float end, float percent)
	{
		return start + percent * end;
	}

	// Scaleform doesn't seem to provide access to any sort of RTC
	// Provide one here, via clock(), which is supposed be backed by QueryPerformanceCounter since VC2015.
	// See https://news.ycombinator.com/item?id=7924333
	class VRInputScaleform_GetClock : public GFxFunctionHandler
	{
	public:
		virtual void	Invoke(Args * args)
		{
			args->result->SetNumber(clock());
		}
	};

	class VRInputScaleform_TriggerHapticPulse : public GFxFunctionHandler
	{
	public:
		virtual void	Invoke(Args * args)
		{
			ASSERT(args->numArgs == 2);
			int controllerRole = args->args[0].GetNumber();
			double strength = args->args[1].GetNumber();

			vr::IVRSystem* vrSystem = OpenVRHookMgr::GetInstance()->GetVRSystem();
			vr::TrackedDeviceIndex_t deviceIndex = vrSystem->GetTrackedDeviceIndexForControllerRole((vr::ETrackedControllerRole)controllerRole);
			vrSystem->TriggerHapticPulse(deviceIndex, 0, lerp(0, 3999, strength));
		}
	};

	bool RegisterFuncs(GFxMovieView * view, GFxValue * plugin) {
		//_MESSAGE("Registering scaleform functions");

		RegisterFunction<VRInputScaleform_ShutoffButtonEventsToGame>(plugin, view, "ShutoffButtonEventsToGame");
		RegisterFunction<VRInputScaleform_RegisterInputHandler>(plugin, view, "RegisterInputHandler");
		RegisterFunction<VRInputScaleform_UnregisterInputHandler>(plugin, view, "UnregisterInputHandler");
		RegisterFunction<VRInputScaleform_GetClock>(plugin, view, "GetClock");
		RegisterFunction<VRInputScaleform_TriggerHapticPulse>(plugin, view, "TriggerHapticPulse");

		return true;
	}
}