
"use strict";

/* =========================================================
   TIMEWISE HTTP CONTROL
   Browser -> HTTP Gateway -> ESP32

   Commands:
       POST /api/control

   Device status:
       GET /api/status

   WebSocket is no longer used.
   ========================================================= */

/* =========================================================
   Element references
   ========================================================= */

const el = id => document.getElementById(id);

const connection = el("connection");

// Music
const musicEnabled = el("musicEnabled");
const musicTitle = el("musicTitle");
const musicArtist = el("musicArtist");
const musicAlbum = el("musicAlbum");
const musicState = el("musicState");
const musicPosition = el("musicPosition");
const musicDuration = el("musicDuration");
const musicProgressBar = el("musicProgressBar");
const musicVolume = el("musicVolume");
const musicVolumeValue = el("musicVolumeValue");

const musicPreviousButton = el("musicPreviousButton");
const musicPlayButton = el("musicPlayButton");
const musicPauseButton = el("musicPauseButton");
const musicNextButton = el("musicNextButton");
const musicStopButton = el("musicStopButton");

// System volume
const volume = el("volume");
const volumeValue = el("volumeValue");

// Brightness
const brightness = el("brightness");
const brightnessValue = el("brightnessValue");
const effectiveBrightness = el("effectiveBrightness");
const dayMode = el("dayMode");

// Alarm
const alarmTime = el("alarmTime");
const alarmEnabled = el("alarmEnabled");
const alarmStatus = el("alarmStatus");
const snoozeStatus = el("snoozeStatus");
const snoozeMinutes = el("snoozeMinutes");
const snoozeButton = el("snoozeButton");
const stopButton = el("stopButton");

/* =========================================================
   Configuration
   ========================================================= */

const STATUS_INTERVAL_MS = 3000;
const COMMAND_TIMEOUT_MS = 5000;
const SLIDER_DELAY_MS = 200;

/* =========================================================
   Local state
   ========================================================= */

let volumeTimer = null;
let brightnessTimer = null;
let musicVolumeTimer = null;

let currentMusicState = "stopped";
let currentMusicPosition = 0;
let currentMusicDuration = 0;

let statusBusy = false;
let deviceConnected = false;

// Prevent status polling from overwriting values
// currently being changed by the user.
const pendingControls = new Map();
const PENDING_CONTROL_MS = 5000;

/* =========================================================
   Helpers
   ========================================================= */

function clamp(value, minimum, maximum) {
    return Math.max(
        minimum,
        Math.min(maximum, value)
    );
}

function formatDuration(seconds) {
    seconds = Math.max(
        0,
        Math.floor(Number(seconds) || 0)
    );

    const minutes = Math.floor(seconds / 60);
    const remaining = seconds % 60;

    return minutes + ":" +
        String(remaining).padStart(2, "0");
}

function formatSnooze(until) {
    if (!until) {
        return "Not scheduled";
    }

    const date = new Date(Number(until) * 1000);

    if (Number.isNaN(date.getTime())) {
        return "Not scheduled";
    }

    return date.toLocaleTimeString([], {
        hour: "2-digit",
        minute: "2-digit"
    });
}

/* =========================================================
   Connection status
   ========================================================= */

function setConnected(connected) {
    deviceConnected = connected;

    connection.textContent =
        connected ? "Connected" : "Disconnected";

    connection.classList.toggle(
        "connected",
        connected
    );
}

/* =========================================================
   HTTP request helper
   ========================================================= */

async function requestJSON(
    path,
    method = "GET",
    data = null
) {
    const controller = new AbortController();

    const timeout = setTimeout(
        () => controller.abort(),
        COMMAND_TIMEOUT_MS
    );

    try {
        const options = {
            method: method,
            cache: "no-store",
            signal: controller.signal,
            headers: {
                "Accept": "application/json"
            }
        };

        if (data !== null) {
            options.headers["Content-Type"] =
                "application/json";

            options.body = JSON.stringify(data);
        }

        const response = await fetch(path, options);

        if (!response.ok) {
            throw new Error(
                "HTTP " + response.status
            );
        }

        return await response.json();
    }
    finally {
        clearTimeout(timeout);
    }
}

/* =========================================================
   Send command to gateway

   IMPORTANT:
   A successful response means the command was
   accepted by the gateway.

   It does not necessarily mean the ESP32 has
   executed the command.
   ========================================================= */

async function send(command) {
    try {
        const result = await requestJSON(
            "/api/control",
            "POST",
            command
        );

        if (result.accepted !== true) {
            throw new Error(
                "Command was not accepted"
            );
        }

        console.log(
            "Command queued:",
            command,
            result
        );

        return true;
    }
    catch (error) {
        console.error(
            "Command failed:",
            command,
            error
        );

        return false;
    }
}

/* =========================================================
   Prevent controls from being overwritten immediately
   after a user changes their values.

   Once ESP32 reports the requested value, normal
   status updates resume.

   A timeout also prevents a control from being
   permanently locked.
   ========================================================= */

function markPending(name, value) {
    pendingControls.set(name, {
        value: value,
        until: Date.now() + PENDING_CONTROL_MS
    });
}

function shouldApply(name, actualValue) {
    const pending = pendingControls.get(name);

    if (!pending) {
        return true;
    }

    if (actualValue === pending.value) {
        pendingControls.delete(name);
        return true;
    }

    if (Date.now() >= pending.until) {
        pendingControls.delete(name);
        return true;
    }

    return false;
}

/* =========================================================
   Music progress display
   ========================================================= */

function updateMusicProgress() {
    musicPosition.textContent =
        formatDuration(currentMusicPosition);

    musicDuration.textContent =
        formatDuration(currentMusicDuration);

    const percent = currentMusicDuration > 0
        ? clamp(
            currentMusicPosition /
            currentMusicDuration * 100,
            0,
            100
        )
        : 0;

    musicProgressBar.style.width =
        percent + "%";
}

/* =========================================================
   Music buttons
   ========================================================= */

function updateMusicButtons() {
    const enabled = musicEnabled.checked;

    const active = [
        "connecting",
        "buffering",
        "playing",
        "paused"
    ].includes(currentMusicState);

    const canPause =
        currentMusicState === "playing" ||
        currentMusicState === "paused";

    musicPlayButton.disabled =
        !enabled || active;

    musicPreviousButton.disabled =
        !enabled;

    musicNextButton.disabled =
        !enabled;

    musicPauseButton.disabled =
        !enabled || !canPause;

    musicStopButton.disabled =
        !active;

    musicPauseButton.textContent =
        currentMusicState === "paused"
            ? "Resume"
            : "Pause";
}

/* =========================================================
   Apply status reported by ESP32
   ========================================================= */

function applyState(state) {
    if (!state || state.type !== "state") {
        return;
    }

    /* -----------------------------------------------------
       Music
       ----------------------------------------------------- */

    if (
        typeof state.music_enabled === "boolean" &&
        shouldApply(
            "music_enabled",
            state.music_enabled
        )
    ) {
        musicEnabled.checked =
            state.music_enabled;
    }

    if (typeof state.music_state === "string") {
        currentMusicState =
            state.music_state;

        musicState.textContent =
            state.music_state;
    }

    if (typeof state.song_title === "string") {
        musicTitle.textContent =
            state.song_title || "Nothing playing";
    }

    if (typeof state.artist === "string") {
        musicArtist.textContent =
            state.artist || "--";
    }

    if (typeof state.album === "string") {
        musicAlbum.textContent =
            state.album || "--";
    }

    if (typeof state.position === "number") {
        currentMusicPosition = Math.max(
            0,
            state.position
        );
    }

    if (typeof state.duration === "number") {
        currentMusicDuration = Math.max(
            0,
            state.duration
        );
    }

    if (
        typeof state.music_volume === "number" &&
        shouldApply(
            "music_volume",
            state.music_volume
        ) &&
        document.activeElement !== musicVolume
    ) {
        musicVolume.value =
            state.music_volume;

        musicVolumeValue.textContent =
            state.music_volume + "%";
    }

    updateMusicProgress();
    updateMusicButtons();

    /* -----------------------------------------------------
       System volume
       ----------------------------------------------------- */

    if (
        typeof state.volume === "number" &&
        shouldApply(
            "volume",
            state.volume
        ) &&
        document.activeElement !== volume
    ) {
        volume.value = state.volume;

        volumeValue.textContent =
            state.volume + "%";
    }

    /* -----------------------------------------------------
       Brightness
       ----------------------------------------------------- */

    if (
        typeof state.brightness === "number" &&
        shouldApply(
            "brightness",
            state.brightness
        ) &&
        document.activeElement !== brightness
    ) {
        brightness.value =
            state.brightness;

        brightnessValue.textContent =
            state.brightness + "%";
    }

    if (
        typeof state.effective_brightness === "number"
    ) {
        effectiveBrightness.textContent =
            state.effective_brightness + "%";
    }

    if (typeof state.is_day === "boolean") {
        dayMode.textContent =
            state.is_day ? "Day" : "Night";
    }

    /* -----------------------------------------------------
       Alarm
       ----------------------------------------------------- */

    if (
        typeof state.alarm_hour === "number" &&
        typeof state.alarm_minute === "number"
    ) {
        const formattedTime =
            String(state.alarm_hour).padStart(2, "0") +
            ":" +
            String(state.alarm_minute).padStart(2, "0");

        if (
            shouldApply("alarm_time", formattedTime) &&
            document.activeElement !== alarmTime
        ) {
            alarmTime.value = formattedTime;
        }
    }

    if (
        typeof state.alarm_enabled === "boolean" &&
        shouldApply(
            "alarm_enabled",
            state.alarm_enabled
        )
    ) {
        alarmEnabled.checked =
            state.alarm_enabled;
    }

    if (typeof state.snooze_minutes === "number") {
        snoozeMinutes.value =
            String(state.snooze_minutes);
    }

    if (typeof state.alarm_ringing === "boolean") {
        alarmStatus.textContent =
            state.alarm_ringing
                ? "Ringing"
                : "Idle";

        snoozeButton.disabled =
            !state.alarm_ringing;
    }

    if (typeof state.snooze_pending === "boolean") {
        snoozeStatus.textContent =
            state.snooze_pending
                ? "Until " +
                  formatSnooze(state.snooze_until)
                : "Not scheduled";
    }

    if (
        typeof state.alarm_ringing === "boolean" &&
        typeof state.snooze_pending === "boolean"
    ) {
        stopButton.disabled =
            !state.alarm_ringing &&
            !state.snooze_pending;
    }
}

/* =========================================================
   GET /api/status

   Read the latest ESP32 state from the HTTP gateway.

   Poll every 3 seconds.
   ========================================================= */

async function refreshState() {
    if (statusBusy) {
        return;
    }

    statusBusy = true;

    try {
        const state = await requestJSON(
            "/api/status"
        );

        // Gateway availability does not imply
        // ESP32 availability.
        setConnected(
            state.device_online === true
        );

        applyState(state);
    }
    catch (error) {
        console.error(
            "HTTP status request failed:",
            error
        );

        setConnected(false);
    }
    finally {
        statusBusy = false;
    }
}

/* =========================================================
   Weather refresh

   This function can be connected to a weather
   refresh button later.
   ========================================================= */

async function refreshWeather() {
    try {
        const result = await requestJSON(
            "/api/weather/update",
            "POST"
        );

        console.log(
            "Weather refresh requested:",
            result
        );

        return true;
    }
    catch (error) {
        console.error(
            "Weather refresh failed:",
            error
        );

        return false;
    }
}

/* =========================================================
   Music controls

   These HTTP commands are prepared for the
   later music implementation.
   ========================================================= */

musicEnabled.addEventListener("change", () => {
    const enabled = musicEnabled.checked;

    markPending("music_enabled", enabled);

    send({
        cmd: "music_enable",
        enabled: enabled
    });

    updateMusicButtons();
});

musicVolume.addEventListener("input", () => {
    const value = Number(musicVolume.value);

    musicVolumeValue.textContent =
        value + "%";

    markPending("music_volume", value);

    clearTimeout(musicVolumeTimer);

    musicVolumeTimer = setTimeout(() => {
        send({
            cmd: "music_volume",
            value: value
        });
    }, SLIDER_DELAY_MS);
});

musicPlayButton.addEventListener("click", () => {
    send({
        cmd: "music_play_random"
    });
});

musicPreviousButton.addEventListener("click", () => {
    send({
        cmd: "music_previous"
    });
});

musicNextButton.addEventListener("click", () => {
    send({
        cmd: "music_next"
    });
});

musicStopButton.addEventListener("click", () => {
    send({
        cmd: "music_stop"
    });
});

musicPauseButton.addEventListener("click", () => {
    send({
        cmd: currentMusicState === "paused"
            ? "music_resume"
            : "music_pause"
    });
});

/* =========================================================
   System volume
   ========================================================= */

volume.addEventListener("input", () => {
    const value = Number(volume.value);

    volumeValue.textContent =
        value + "%";

    markPending("volume", value);

    clearTimeout(volumeTimer);

    volumeTimer = setTimeout(() => {
        send({
            cmd: "volume",
            value: value
        });
    }, SLIDER_DELAY_MS);
});

/* =========================================================
   Display brightness
   ========================================================= */

brightness.addEventListener("input", () => {
    const value = Number(brightness.value);

    brightnessValue.textContent =
        value + "%";

    markPending("brightness", value);

    clearTimeout(brightnessTimer);

    brightnessTimer = setTimeout(() => {
        send({
            cmd: "brightness",
            value: value
        });
    }, SLIDER_DELAY_MS);
});

/* =========================================================
   Alarm
   ========================================================= */

function sendAlarm() {
    const parts = alarmTime.value.split(":");

    if (parts.length !== 2) {
        return;
    }

    const hour = Number(parts[0]);
    const minute = Number(parts[1]);

    if (
        !Number.isInteger(hour) ||
        !Number.isInteger(minute) ||
        hour < 0 ||
        hour > 23 ||
        minute < 0 ||
        minute > 59
    ) {
        return;
    }

    const enabled = alarmEnabled.checked;

    markPending("alarm_time", alarmTime.value);
    markPending("alarm_enabled", enabled);

    send({
        cmd: "alarm",
        hour: hour,
        minute: minute,
        enabled: enabled
    });
}

alarmTime.addEventListener(
    "change",
    sendAlarm
);

alarmEnabled.addEventListener(
    "change",
    sendAlarm
);

/* =========================================================
   Snooze
   ========================================================= */

snoozeButton.addEventListener("click", () => {
    const minutes = Number(
        snoozeMinutes.value
    );

    send({
        cmd: "snooze",
        minutes: minutes
    });
});

/* =========================================================
   Stop alarm
   ========================================================= */

stopButton.addEventListener("click", () => {
    send({
        cmd: "stop"
    });
});

/* =========================================================
   Local music progress timer

   Only updates the displayed progress between
   status responses.

   Actual playback is managed by ESP32.
   ========================================================= */

setInterval(() => {
    if (
        deviceConnected &&
        currentMusicState === "playing" &&
        currentMusicDuration > 0 &&
        currentMusicPosition < currentMusicDuration
    ) {
        currentMusicPosition++;

        updateMusicProgress();
    }
}, 1000);

/* =========================================================
   Periodic HTTP status polling
   ========================================================= */

setInterval(
    refreshState,
    STATUS_INTERVAL_MS
);

/* =========================================================
   Initialization
   ========================================================= */

updateMusicButtons();
updateMusicProgress();
setConnected(false);

refreshState();
