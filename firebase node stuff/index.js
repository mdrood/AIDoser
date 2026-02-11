"use strict";

/**
 * RTDB -> FCM push
 * Trigger: devices/{deviceId}/notifications/{notifId}
 * Tokens:  devices/{deviceId}/pushTokens/{token}
 *
 * Offline monitor:
 * - Runs every minute
 * - If now - state.lastSeen > OFFLINE_MS:
 *   - set state.online=false
 *   - write a notification to devices/{deviceId}/notifications (so push function sends it)
 */

const functions = require("firebase-functions/v1"); // IMPORTANT: v1 API for RTDB + schedule
const admin = require("firebase-admin");

admin.initializeApp();

/** ------------------ PUSH ON NOTIFICATION ------------------ **/
exports.pushOnDeviceNotification = functions.database
  .ref("devices/{deviceId}/notifications/{notifId}")
  .onCreate(async (snap, ctx) => {
    const deviceId = String(ctx.params.deviceId || "");
    const notifId = String(ctx.params.notifId || "");
    const notif = snap.val() || {};

    const title = String(notif.title || "Reef Doser");
    const body = String(notif.body || notif.message || "Device update");
    const severity = String(notif.severity || "info").toLowerCase();

    // Push only important ones (change if you want more noise)
    const pushIf = ["warning", "error", "critical"];
    if (!pushIf.includes(severity)) return null;

    const tokensSnap = await admin
      .database()
      .ref(`devices/${deviceId}/pushTokens`)
      .once("value");

    const tokensObj = tokensSnap.val() || {};
    const tokens = Object.keys(tokensObj).filter(Boolean);

    if (!tokens.length) {
      console.log(`No tokens for device ${deviceId}`);
      return null;
    }

    const message = {
      tokens,
      notification: { title, body },
      data: {
        deviceId: String(deviceId),
        notifId: String(notifId),
        severity: String(severity),
        type: String(notif.type || "")
      },
      webpush: {
        notification: {
          icon: "/icon-192.png",
          badge: "/icon-192.png",
          tag: `reef-${deviceId}`
        }
      }
    };

    const resp = await admin.messaging().sendEachForMulticast(message);
    console.log(`Push sent: ok=${resp.successCount}, fail=${resp.failureCount}`);

    // Remove dead tokens
    const deletes = [];
    resp.responses.forEach((r, idx) => {
      if (!r.success) {
        const code = r.error?.code || "";
        if (
          code === "messaging/registration-token-not-registered" ||
          code === "messaging/invalid-registration-token"
        ) {
          const t = tokens[idx];
          deletes.push(admin.database().ref(`devices/${deviceId}/pushTokens/${t}`).remove());
          console.log("Removed invalid token:", t);
        } else {
          console.log("Send error:", code);
        }
      }
    });
    await Promise.all(deletes);

    // Mark notification as pushed (optional)
    await admin
      .database()
      .ref(`devices/${deviceId}/notifications/${notifId}/pushedAt`)
      .set(Date.now());

    return null;
  });

/** ------------------ OFFLINE MONITOR (CLOUD-ONLY) ------------------ **/

// How long until we consider it offline
const OFFLINE_MS = 4 * 60 * 1000; // 4 minutes

exports.offlineMonitor = functions.pubsub
  .schedule("every 1 minutes")
  .timeZone("America/Chicago")
  .onRun(async () => {
    const db = admin.database();
    const now = Date.now();

    const snap = await db.ref("devices").once("value");
    const devices = snap.val() || {};

    const updates = {};
    const notifWrites = [];

    for (const deviceId of Object.keys(devices)) {
      const dev = devices[deviceId] || {};
      const state = dev.state || {};

      const lastSeen = Number(state.lastSeen || 0);
      if (!lastSeen) continue; // never seen it

      const stale = (now - lastSeen) > OFFLINE_MS;

      // Use offlineSince as the latch (NOT state.online)
      const offlineSince = Number(state.offlineSince || 0);

      // ---- OFFLINE: first time we detect staleness ----
      if (stale && !offlineSince) {
        updates[`devices/${deviceId}/state/online`] = false;
        updates[`devices/${deviceId}/state/offlineSince`] = now;

        notifWrites.push(
          db.ref(`devices/${deviceId}/notifications`).push({
            title: `${deviceId} offline`,
            body: `No heartbeat since ${new Date(lastSeen).toLocaleString("en-US", { timeZone: "America/Chicago" })}`,
            severity: "critical",
            type: "device_offline",
            ts: now,
            deviceId
          })
        );
      }

      // ---- ONLINE AGAIN: staleness cleared and we were previously offline ----
      if (!stale && offlineSince) {
        updates[`devices/${deviceId}/state/online`] = true;
        updates[`devices/${deviceId}/state/onlineSince`] = now;
        updates[`devices/${deviceId}/state/offlineSince`] = 0; // safer than null (avoids rule issues)

        // If you DON'T want "back online" pushes, delete this block
        notifWrites.push(
          db.ref(`devices/${deviceId}/notifications`).push({
            title: `${deviceId} online`,
            body: `Device is back online.`,
            severity: "warning",
            type: "device_online",
            ts: now,
            deviceId
          })
        );
      }
    }

    if (Object.keys(updates).length) {
      await db.ref().update(updates);
    }
    if (notifWrites.length) {
      await Promise.all(notifWrites);
    }

    return null;
  });
  // ---- Prune doseRuns older than 365 days ------------------------

const PRUNE_KEEP_DAYS = 365;
const PRUNE_BATCH_SIZE = 500;

// Runs daily at 03:15am Chicago time (quiet hours)
exports.pruneDoseRuns = functions.pubsub
  .schedule("15 3 * * *")
  .timeZone("America/Chicago")
  .onRun(async () => {
    const db = admin.database();
    const cutoff = Date.now() - PRUNE_KEEP_DAYS * 24 * 60 * 60 * 1000;

    const devicesSnap = await db.ref("devices").once("value");
    const devices = devicesSnap.val() || {};

    let totalDeleted = 0;

    for (const deviceId of Object.keys(devices)) {
      // Keep deleting in batches until no more old rows
      while (true) {
        const qSnap = await db
          .ref(`devices/${deviceId}/doseRuns`)
          .orderByChild("ts")
          .endAt(cutoff)
          .limitToFirst(PRUNE_BATCH_SIZE)
          .once("value");

        const old = qSnap.val();
        if (!old) break;

        const updates = {};
        for (const runId of Object.keys(old)) {
          updates[runId] = null;
        }

        await db.ref(`devices/${deviceId}/doseRuns`).update(updates);

        const deletedNow = Object.keys(updates).length;
        totalDeleted += deletedNow;

        // If we deleted fewer than batch size, we're done for this device
        if (deletedNow < PRUNE_BATCH_SIZE) break;
      }
    }

    console.log(`pruneDoseRuns: deleted ${totalDeleted} records older than ${PRUNE_KEEP_DAYS} days`);
    return null;
  });

  // ---- Prune notifications older than N days ----------------------

const PRUNE_NOTIF_KEEP_DAYS = 30;      // change to 60/90/365 if you want
const PRUNE_NOTIF_BATCH_SIZE = 500;

exports.pruneNotifications = functions.pubsub
  .schedule("25 3 * * *")              // daily 03:25am
  .timeZone("America/Chicago")
  .onRun(async () => {
    const db = admin.database();
    const cutoff = Date.now() - PRUNE_NOTIF_KEEP_DAYS * 24 * 60 * 60 * 1000;

    const devicesSnap = await db.ref("devices").once("value");
    const devices = devicesSnap.val() || {};

    let totalDeleted = 0;

    for (const deviceId of Object.keys(devices)) {
      while (true) {
        const qSnap = await db
          .ref(`devices/${deviceId}/notifications`)
          .orderByChild("ts")
          .endAt(cutoff)
          .limitToFirst(PRUNE_NOTIF_BATCH_SIZE)
          .once("value");

        const old = qSnap.val();
        if (!old) break;

        const updates = {};
        for (const notifId of Object.keys(old)) {
          updates[notifId] = null;
        }

        await db.ref(`devices/${deviceId}/notifications`).update(updates);

        const deletedNow = Object.keys(updates).length;
        totalDeleted += deletedNow;

        if (deletedNow < PRUNE_NOTIF_BATCH_SIZE) break;
      }
    }

    console.log(`pruneNotifications: deleted ${totalDeleted} notifications older than ${PRUNE_NOTIF_KEEP_DAYS} days`);
    return null;
  });

  /** ------------------ SENSOR RANGE ALERTS (RTDB -> notifications) ------------------ **/


  /** ------------------ SENSOR RANGE ALERTS (RTDB -> notifications) ------------------ **/

const SENSOR_LIMITS = {
  pH:    { min: 7.5,   max: 10.5,  label: "pH" },
  tempF: { min: 75.0,  max: 82.0,  label: "Temp (°F)" },
  sg:    { min: 1.023, max: 1.027, label: "Salinity (SG)" },
};

// avoid spam while a value stays bad
const SENSOR_COOLDOWN_MS = 30 * 60 * 1000; // 30 minutes

exports.sensorRangeAlerts = functions.database
  .ref("devices/{deviceId}/sensors/{sensorKey}")
  .onWrite(async (change, ctx) => {
    const deviceId = String(ctx.params.deviceId || "");
    const sensorKey = String(ctx.params.sensorKey || "");

    const limits = SENSOR_LIMITS[sensorKey];
    if (!limits) return null;

    const now = Date.now();

    // Ignore deletes
    const raw = change.after.val();
    if (raw === null || raw === undefined) return null;

    // Support either plain number OR { value: <num>, ts: <ms> }
    const value = Number(
      (typeof raw === "object" && raw !== null && raw.value !== undefined) ? raw.value : raw
    );
    if (!Number.isFinite(value)) return null;

    const outLow = value < limits.min;
    const outHigh = value > limits.max;
    const outOfRange = outLow || outHigh;

    // Latch state: devices/<id>/state/sensorAlerts/<sensorKey>
    const latchRef = admin.database().ref(`devices/${deviceId}/state/sensorAlerts/${sensorKey}`);
    const latchSnap = await latchRef.once("value");
    const latch = latchSnap.val() || {};

    const wasOut = !!latch.outOfRange;
    const lastSentAt = Number(latch.lastSentAt || 0);

    // -------- BACK TO NORMAL (transition only) --------
    if (!outOfRange) {
      // If we were previously out-of-range, send "back to normal" ONCE
      if (wasOut) {
        const title = `${deviceId} ${limits.label} normal`;
        const body = `${limits.label} is back in range at ${value} (expected ${limits.min}–${limits.max}).`;

        await admin.database().ref(`devices/${deviceId}/notifications`).push({
          title,
          body,
          deviceId,
          severity: "warning", // choose "warning" or "critical" — warning is usually nicer
          type: `sensor_${sensorKey}_back_normal`,
          sensorKey,
          value,
          min: limits.min,
          max: limits.max,
          ts: now
        });

        await latchRef.set({ outOfRange: false, lastSentAt: now, lastValue: value });
      } else {
        await latchRef.update({ lastValue: value });
      }
      return null;
    }

    // -------- OUT OF RANGE --------
    // Alert on transition OR after cooldown
    const cooldownPassed = (now - lastSentAt) > SENSOR_COOLDOWN_MS;
    const shouldAlert = (!wasOut) || cooldownPassed;

    if (!shouldAlert) {
      await latchRef.update({ outOfRange: true, lastValue: value });
      return null;
    }

    const which = outLow ? "LOW" : "HIGH";
    const title = `${deviceId} ${limits.label} ${which}`;
    const body = `${limits.label} is ${value}. Expected ${limits.min}–${limits.max}.`;

    await admin.database().ref(`devices/${deviceId}/notifications`).push({
      title,
      body,
      deviceId,
      severity: "critical",
      type: `sensor_${sensorKey}_out_of_range`,
      sensorKey,
      value,
      min: limits.min,
      max: limits.max,
      ts: now
    });

    await latchRef.set({
      outOfRange: true,
      lastSentAt: now,
      lastValue: value
    });

    return null;
  });