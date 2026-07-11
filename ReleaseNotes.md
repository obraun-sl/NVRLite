## Release Notes

#### v0.2.5
- Fix crash/instability when starting or stopping recording from the display window (C/S keys)
- Fix recording control getting stuck on a stream after a failed start (would reject all further start requests)
- Recording status now stays "recording" until the post-buffer is fully written and the file is closed
- `/record/start` now returns 202 (accepted) instead of 500 when the file name is not known yet, though recording did start
- Recording folder is now created recursively (nested paths work)
- Bound memory use of the pre-record buffer for streams with missing timestamps
- Faster, more reliable stream stop and application shutdown
- Fix per-camera name overlay not showing on the display grid
- Accept integer values for pre/post buffering time in the config

#### v0.2.4
- Add log_level option in json config file
- Improve HTTP server for START/STOP rec response.
- Add File removal and File status/listing in API REST

#### v0.2.3
- Add support for Windows

#### v0.2.2
- Add record/stream status in REST API (GET /stream/status(?stream_id=xxx))  

#### v0.2.1
- Change Nosig frame to specific text
- Fix mutex lock when stop called when no start
- Add base recording folder (default "./")
- fix pre buffering time settings in recorder
- Add post buffering time settings (N seconds after STOP)

#### v0.2.0
- Add REST control to start/stop stream 
- Add autostart mode in json

#### 0.1.0
- Initial Version
 
 
