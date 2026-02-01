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

// How long until we consider it offline (choose what you want)
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
      const online = !!state.online;

      // If we never saw it, skip
      if (!lastSeen) continue;

      const stale = (now - lastSeen) > OFFLINE_MS;

      // Only notify ONCE per offline event
      const offlineSince = Number(state.offlineSince || 0);
      const alreadyOffline = !online;

      // Transition online -> offline
      if (online && stale) {
        updates[`devices/${deviceId}/state/online`] = false;
        updates[`devices/${deviceId}/state/offlineSince`] = now;

        // Write a notification (your push function will deliver it)
        const title = `${deviceId} offline`;
        const body = `No heartbeat since ${new Date(lastSeen).toLocaleString("en-US", { timeZone: "America/Chicago" })}`;

        notifWrites.push(
          db.ref(`devices/${deviceId}/notifications`).push({
            title,
            body,
            severity: "critical",
            type: "device_offline",
            ts: now,
            deviceId
          })
        );
      }

      // Optional: when it comes back (stale=false) flip online true + notify once
      // Only do this if you WANT "back online" pushes.
      const onlineSince = Number(state.onlineSince || 0);
      const shouldBeOnline = !stale;

      if (alreadyOffline && shouldBeOnline) {
        updates[`devices/${deviceId}/state/online`] = true;
        updates[`devices/${deviceId}/state/onlineSince`] = now;
        updates[`devices/${deviceId}/state/offlineSince`] = null;

        // Comment this block out if you don't want "back online" pushes
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