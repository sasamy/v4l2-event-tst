# v4l2-event-tst

Testing the ability of the tw6869 driver to send events to user space.

The example captures video from the camera and shows it using the imxg2dvideosink plugin.
A separate thread waits for events from the capture device:
* if the V4L2_EVENT_SOURCE_CHANGE event is received, the pipeline is restarted completely
* if the V4L2_EVENT_MOTION_DET event is received, a second video sinc (which writes the video to the file) is dynamically added and removed
