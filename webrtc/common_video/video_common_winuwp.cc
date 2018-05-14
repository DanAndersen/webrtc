#include "video_common_winuwp.h"

Windows::UI::Core::CoreDispatcher^ webrtc::VideoCommonWinUWP::windowDispatcher;
Windows::Perception::Spatial::ISpatialCoordinateSystem^ webrtc::VideoCommonWinUWP::spatialCoordinateSystem;

void webrtc::VideoCommonWinUWP::SetCoreDispatcher(Windows::UI::Core::CoreDispatcher^ inWindowDispatcher)
{
	windowDispatcher = inWindowDispatcher;
}

Windows::UI::Core::CoreDispatcher^ webrtc::VideoCommonWinUWP::GetCoreDispatcher()
{
	return windowDispatcher;
}

void webrtc::VideoCommonWinUWP::SetSpatialCoordinateSystem(Windows::Perception::Spatial::ISpatialCoordinateSystem^ inSpatialCoordinateSystem)
{
	spatialCoordinateSystem = inSpatialCoordinateSystem;
}

Windows::Perception::Spatial::ISpatialCoordinateSystem^ webrtc::VideoCommonWinUWP::GetSpatialCoordinateSystem()
{
	return spatialCoordinateSystem;
}