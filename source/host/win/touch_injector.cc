//
// Aspia Project
// Copyright (C) 2016-2025 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "host/win/touch_injector.h"

#include "base/logging.h"
#include "base/desktop/geometry.h"
#include "base/desktop/win/screen_capture_utils.h"

#include <vector>

namespace host {

namespace {

const uint32_t kMaxSimultaneousTouchCount = 10;

// This is used to reinject all points that have not changed as "move"ed points, even if they have
// not actually moved.
// This is required for multi-touch to work, e.g. pinching and zooming gestures (handled by apps)
// won't work without reinjecting the points, even though the user moved only one finger and held
// the other finger in place.
void appendMapValuesToVector(
    std::map<uint32_t, OWN_POINTER_TOUCH_INFO>* touches_in_contact,
    std::vector<OWN_POINTER_TOUCH_INFO>* output_vector)
{
    for (auto& id_and_pointer_touch_info : *touches_in_contact)
    {
        OWN_POINTER_TOUCH_INFO& pointer_touch_info = id_and_pointer_touch_info.second;
        output_vector->push_back(pointer_touch_info);
    }
}

void convertToPointerTouchInfoImpl(
    const proto::TouchEventPoint& touch_point, OWN_POINTER_TOUCH_INFO* pointer_touch_info)
{
    pointer_touch_info->touchMask =
        OWN_TOUCH_MASK_CONTACTAREA | OWN_TOUCH_MASK_ORIENTATION;
    pointer_touch_info->touchFlags = TOUCH_FLAG_NONE;

    // Although radius_{x,y} can be undefined (i.e. has_radius_{x,y} == false),
    // the default value (0.0) will set the area correctly.
    // MSDN mentions that if the digitizer does not detect the size of the touch
    // point, rcContact should be set to 0 by 0 rectangle centered at the
    // coordinate.
    pointer_touch_info->rcContact.left =
        static_cast<LONG>(touch_point.x() - touch_point.radius_x());
    pointer_touch_info->rcContact.top = static_cast<LONG>(touch_point.y() - touch_point.radius_y());
    pointer_touch_info->rcContact.right =
        static_cast<LONG>(touch_point.x() + touch_point.radius_x());
    pointer_touch_info->rcContact.bottom =
        static_cast<LONG>(touch_point.y() + touch_point.radius_y());

    pointer_touch_info->orientation = static_cast<UINT32>(touch_point.angle());

    if (touch_point.pressure() != 0)
    {
        pointer_touch_info->touchMask |= OWN_TOUCH_MASK_PRESSURE;
        const float kMinimumPressure = 0.0;
        const float kMaximumPressure = 1.0;
        const float clamped_touch_point_pressure =
            std::max(kMinimumPressure,
                     std::min(kMaximumPressure, touch_point.pressure()));

        const float kWindowsMaxTouchPressure = 1024;  // Defined in MSDN.
        const float pressure =
            clamped_touch_point_pressure * kWindowsMaxTouchPressure;
        pointer_touch_info->pressure = static_cast<UINT32>(pressure);
    }

    pointer_touch_info->pointerInfo.pointerType = PT_TOUCH;
    pointer_touch_info->pointerInfo.pointerId = touch_point.id();
    pointer_touch_info->pointerInfo.ptPixelLocation.x = static_cast<LONG>(touch_point.x());
    pointer_touch_info->pointerInfo.ptPixelLocation.y = static_cast<LONG>(touch_point.y());
}

// The caller should set memset(0) the struct and set
// pointer_touch_info->pointerInfo.pointerFlags.
void convertToPointerTouchInfo(
    const proto::TouchEventPoint& touch_point, OWN_POINTER_TOUCH_INFO* pointer_touch_info)
{
    // TODO(zijiehe): Use GetFullscreenTopLeft() once
    // https://chromium-review.googlesource.com/c/581951/ is submitted.
    base::Point top_left = base::ScreenCaptureUtils::fullScreenRect().topLeft();
    if (top_left == base::Point(0, 0))
    {
        convertToPointerTouchInfoImpl(touch_point, pointer_touch_info);
        return;
    }

    proto::TouchEventPoint point(touch_point);
    point.set_x(point.x() + top_left.x());
    point.set_y(point.y() + top_left.y());

    convertToPointerTouchInfoImpl(point, pointer_touch_info);
}

} // namespace

//--------------------------------------------------------------------------------------------------
TouchInjector::TouchInjector()
{
    user32_library_ = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!user32_library_)
    {
        PLOG(LS_ERROR) << "LoadLibraryExW failed";
        return;
    }

    initialize_touch_injection_ = reinterpret_cast<InitializeTouchInjectionFunction>(
        GetProcAddress(user32_library_, "InitializeTouchInjection"));
    if (!initialize_touch_injection_)
    {
        PLOG(LS_ERROR) << "GetProcAddress failed";
        return;
    }

    inject_touch_input_ = reinterpret_cast<InjectTouchInputFunction>(
        GetProcAddress(user32_library_, "InjectTouchInput"));
    if (!inject_touch_input_)
    {
        PLOG(LS_ERROR) << "GetProcAddress failed";
        return;
    }

    if (!initialize_touch_injection_(kMaxSimultaneousTouchCount, OWN_TOUCH_FEEDBACK_DEFAULT))
    {
        PLOG(LS_ERROR) << "InitializeTouchInjection failed";
        return;
    }

    initialized_ = true;
}

//--------------------------------------------------------------------------------------------------
TouchInjector::~TouchInjector()
{
    if (user32_library_)
        FreeLibrary(user32_library_);
}

//--------------------------------------------------------------------------------------------------
void TouchInjector::injectTouchEvent(const proto::TouchEvent& event)
{
    if (!initialized_)
        return;

    switch (event.event_type())
    {
        case proto::TouchEvent::TOUCH_POINT_START:
            addNewTouchPoints(event);
            break;
        case proto::TouchEvent::TOUCH_POINT_MOVE:
            moveTouchPoints(event);
            break;
        case proto::TouchEvent::TOUCH_POINT_END:
            endTouchPoints(event);
            break;
        case proto::TouchEvent::TOUCH_POINT_CANCEL:
            cancelTouchPoints(event);
            break;
        default:
            NOTREACHED();
            return;
    }
}

//--------------------------------------------------------------------------------------------------
void TouchInjector::addNewTouchPoints(const proto::TouchEvent& event)
{
    DCHECK_EQ(event.event_type(), proto::TouchEvent::TOUCH_POINT_START);

    std::vector<OWN_POINTER_TOUCH_INFO> touches;
    // Must inject already touching points as move events.
    appendMapValuesToVector(&touches_in_contact_, &touches);

    for (const proto::TouchEventPoint& touch_point : event.touch_points())
    {
        OWN_POINTER_TOUCH_INFO pointer_touch_info;
        memset(&pointer_touch_info, 0, sizeof(pointer_touch_info));

        pointer_touch_info.pointerInfo.pointerFlags =
            OWN_POINTER_FLAG_INRANGE | OWN_POINTER_FLAG_INCONTACT | OWN_POINTER_FLAG_DOWN;
        convertToPointerTouchInfo(touch_point, &pointer_touch_info);
        touches.push_back(pointer_touch_info);

        // All points in the map should be a move point.
        pointer_touch_info.pointerInfo.pointerFlags =
            OWN_POINTER_FLAG_INRANGE | OWN_POINTER_FLAG_INCONTACT | OWN_POINTER_FLAG_UPDATE;
        touches_in_contact_[touch_point.id()] = pointer_touch_info;
    }

    if (inject_touch_input_(static_cast<UINT32>(touches.size()), touches.data()) == 0)
    {
        PLOG(LS_ERROR) << "Failed to inject a touch start event";
    }
}

//--------------------------------------------------------------------------------------------------
void TouchInjector::moveTouchPoints(const proto::TouchEvent& event)
{
    DCHECK_EQ(event.event_type(), proto::TouchEvent::TOUCH_POINT_MOVE);

    for (const proto::TouchEventPoint& touch_point : event.touch_points())
    {
        OWN_POINTER_TOUCH_INFO* pointer_touch_info = &touches_in_contact_[touch_point.id()];
        memset(pointer_touch_info, 0, sizeof(*pointer_touch_info));

        pointer_touch_info->pointerInfo.pointerFlags =
            OWN_POINTER_FLAG_INRANGE | OWN_POINTER_FLAG_INCONTACT | OWN_POINTER_FLAG_UPDATE;
        convertToPointerTouchInfo(touch_point, pointer_touch_info);
    }

    std::vector<OWN_POINTER_TOUCH_INFO> touches;
    // Must inject already touching points as move events.
    appendMapValuesToVector(&touches_in_contact_, &touches);

    if (inject_touch_input_(static_cast<UINT32>(touches.size()), touches.data()) == 0)
    {
        PLOG(LS_ERROR) << "Failed to inject a touch move event";
    }
}

//--------------------------------------------------------------------------------------------------
void TouchInjector::endTouchPoints(const proto::TouchEvent& event)
{
    DCHECK_EQ(event.event_type(), proto::TouchEvent::TOUCH_POINT_END);

    std::vector<OWN_POINTER_TOUCH_INFO> touches;
    for (const proto::TouchEventPoint& touch_point : event.touch_points())
    {
        OWN_POINTER_TOUCH_INFO pointer_touch_info = touches_in_contact_[touch_point.id()];
        pointer_touch_info.pointerInfo.pointerFlags = OWN_POINTER_FLAG_UP;

        touches_in_contact_.erase(touch_point.id());
        touches.push_back(pointer_touch_info);
    }

    appendMapValuesToVector(&touches_in_contact_, &touches);

    if (inject_touch_input_(static_cast<UINT32>(touches.size()), touches.data()) == 0)
    {
        PLOG(LS_ERROR) << "Failed to inject a touch end event";
    }
}

//--------------------------------------------------------------------------------------------------
void TouchInjector::cancelTouchPoints(const proto::TouchEvent& event)
{
    DCHECK_EQ(event.event_type(), proto::TouchEvent::TOUCH_POINT_CANCEL);

    std::vector<OWN_POINTER_TOUCH_INFO> touches;
    for (const proto::TouchEventPoint& touch_point : event.touch_points())
    {
        OWN_POINTER_TOUCH_INFO pointer_touch_info = touches_in_contact_[touch_point.id()];
        pointer_touch_info.pointerInfo.pointerFlags =
            OWN_POINTER_FLAG_UP | OWN_POINTER_FLAG_CANCELED;

        touches_in_contact_.erase(touch_point.id());
        touches.push_back(pointer_touch_info);
    }

    appendMapValuesToVector(&touches_in_contact_, &touches);

    if (inject_touch_input_(static_cast<UINT32>(touches.size()), touches.data()) == 0)
    {
        PLOG(LS_ERROR) << "Failed to inject a touch cancel event";
    }
}

} // namespace host
