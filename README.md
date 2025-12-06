# NVRLite – Lightweight Multi-Camera RTSP NVR

NVRLite is a Unix lightweight, Qt‑based Network Video Recorder designed to handle multiple RTSP video streams with:

- Low‑latency preview via OpenCV
- MP4 recording **without re‑encoding** (H.264 passthrough)
- Pre‑record buffer (N seconds before trigger)
- Auto‑reconnect with “NO SIGNAL” overlay when streams drop
- HTTP (REST) control for starting/stopping recordings per camera

It targets a developer‑friendly workflow similar to known NVRs in the market , but in a lightweight C++ application.

---

## 1. High‑Level Architecture

NVRLite is built around three main components:

1. **Capture layer** – `RtspCaptureThread`
   - One `QThread` per RTSP stream
   - Opens RTSP with FFmpeg, decodes frames for display, and emits encoded H.264 packets for recording
   - Auto‑retry when RTSP is down (every 5 seconds)
   - Emits **NO SIGNAL** frames when offline

2. **Recording layer** – `Mp4RecorderWorker`
   - One recorder instance per stream (can be run in its own `QThread`)
   - Receives encoded packets and writes MP4 files **without re‑encoding**
   - Supports pre‑buffering: when recording starts, it also writes the last *N* seconds of video

3. **Control / HTTP layer** – `HttpDataServer`
   - Runs an embedded HTTP server using **cpp‑httplib**
   - Exposes APIs to start and stop recording for each `stream_id`
   - Tracks last output file per stream and returns it on `/record/stop`

Display uses **OpenCV** windows (no Qt GUI), with a **grid layout** that shows all active cameras in a single window.

The core application uses `QCoreApplication` (no `QMainWindow`, no `QtGui`/`QtWidgets`).

---

## 2. Dependencies

### System packages (Ubuntu / Debian)

You need:

- Qt5 core:
  - `qtbase5-dev`
- FFmpeg and dev headers:
  - `ffmpeg libavcodec-dev libavformat-dev libavutil-dev libswscale-dev`
- OpenCV:
  - `libopencv-dev`
- Build tools:
  - `build-essential cmake pkg-config`

Install (example):

```bash
sudo apt update
sudo apt install -y \
  qtbase5-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  libopencv-dev \
  ffmpeg \
  build-essential cmake pkg-config
```

---


## 3. Build Instructions

```
mkdir build
cd build
cmake ..
make -j8
```

Run:

```
./NVRLite --config config.json
```
See (6) for json configuration

---

## 4. Recording Controls

### 4.1 REST API

#### 4.1.1 Start recording

**Endpoint**

```http
POST /record/start
Content-Type: application/json
```

**Request body**

```json
{
  "stream_id": "stream_1"
}
```

**Behavior**

- Parses JSON.
- If `stream_id` is missing or not a string:
  - Returns `400` and `{"status":"error","message":"Missing or invalid 'stream_id'"}`.
- Otherwise:
  - Emits Qt signal:

    ```cpp
    emit startRecordingRequested(streamId);
    ```

  - Returns:

    ```json
    {
      "status": "ok",
      "stream_id": "stream_1"
    }
    ```

**Notes**

- This **only triggers** the recording request. The actual MP4 file name is decided by `Mp4RecorderWorker` and reported back via `onRecordingStarted`.

#### 4.1.2 Stop recording

**Endpoint**

```http
POST /record/stop
Content-Type: application/json
```

**Request body**

```json
{
  "stream_id": "stream_1"
}
```

**Behavior**

- Parses JSON.
- Emits:

  ```cpp
  emit stopRecordingRequested(streamId);
  ```

- Looks up last known recording file for this `streamId` via `m_lastRecordingFile` (filled in `onRecordingStarted`).

**Responses**

- If a filename is known:

  ```json
  {
    "status": "ok",
    "stream_id": "stream_1",
    "file": "rec_stream_1_2025-12-04_11-43-27.mp4"
  }
  ```

- If no recording file is known yet (e.g. recording never started):

  ```json
  {
    "status": "warning",
    "stream_id": "stream_1",
    "file": null,
    "message": "No known recording file for this stream (maybe never started?)"
  }
  ```

- On JSON parse error or malformed body:

  ```json
  {
    "status": "error",
    "message": "JSON parse error: ..."
  }
  ```

with HTTP `400`.


---

## 5. Display

- A **single OpenCV window** (e.g. `"NVRLite"`) shows all cameras in a **grid**.
- For each frame, capture threads emit:

  ```cpp
  emit frameReady(streamId, bgrMat);
  ```

- A display manager collects frames from all `streamId`s, arranges them into a grid (`cv::hconcat` + `cv::vconcat`), and shows the result.
- When a stream is offline, the capture thread periodically emits a **NO SIGNAL** frame instead.

---

## 6. Configure NVRLite options

### 6.1 Json configuration

The config file must be formatted the following way : 

```json
{
  "streams": [
    { "id": "<name_of_camera>", "url": "<url>" },
    { "id": "<name_of_camera>", "url": "<url>" }
  ],
  "http_port": 8090,
  "autostart":0,
  "display_mode":0,
  "pre_buffering_time":5.0,
  "post_buffering_time":0.5,
  "rec_base_folder":"/home/user/recordings/"   
}
```


- `streams` contains the list of rtsp stream and associated name
- `http_port` defines the REST API port to contact (0 - 65535)
- `autostart` defines if the stream must start at launch
- `display_mode` defines if the display grid is visible  ( 0 = off, 1 = on)
- `pre_buffering_time` defines the time to buffer the packet stream when start is called in seconds ( i.e. will save the last N seconds in the mp4 when the start call is made). This is used to compensate latency
- `post_buffering_time` defines the time to keep recording when stop is called (in seconds) ( i.e. will save N seconds more in the mp4 when the stop call is made)
- `rec_base_folder` defines the folder to save MP4. Will be created if does not exist

Note : Granularity of time is ms inside the app. 

See provided example.json
 
### 6.2 Basic workflow
1. Start NVRLite (QCoreApplication).
2. Capture threads start and either:
   - connect to RTSP and start sending frames and packets, or
   - go into “retry every 5s” + NO SIGNAL display mode.
3. OpenCV window shows all streams in a grid.
4. To start recording for `stream_1`:

   ```bash
   curl -X POST http://<ip>:<port>/record/start \
        -H "Content-Type: application/json" \
        -d '{"stream_id":"stream_1"}'
   ```

5. To stop recording and get the output file path:

   ```bash
   curl -X POST http://<ip>:<port>/record/stop \
        -H "Content-Type: application/json" \
        -d '{"stream_id":"stream_1"}'
   ```

If correct, the response will be  
 
  ```json
  {
    "status": "ok",
    "stream_id": "<camera_name>",
    "file": "<name_of_the_mp4>"
  }
  ```


6. MP4 files are written as:

   ```text
   rec_<streamId>_YYYY-MM-DD_HH-MM-SS.mp4
   ```

---

## 7. Notes & Tips

- Ensure your RTSP source is sending **SPS/PPS** in-band so that FFmpeg can capture `extradata` and the MP4 is playable.
- If you see “dimensions not set” or invalid MP4s:
  - Make sure `StreamInfo.width/height` are updated after the **first decoded frame** (as in the current code).
  - Ensure you pass correct `codec_id` and `extradata` into the MP4 muxer.
- For heavy loads (many cameras), make sure:
  - Each `RtspCaptureThread` runs in its own thread.
  - Each `Mp4RecorderWorker` is in a separate `QThread` to avoid blocking.

---

## 8. Roadmap

- Stream Start/Stop control and autostart
- MQTT controls
- ONVIF device discovery
- Metadata overlay (timestamps, camera name, etc.)
- Export / share segments via HTTP

---

