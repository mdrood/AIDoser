"use strict";

/**
 * Send an FCM push when a new notification is written to RTDB:
 *   devices/{deviceId}/notifications/{notifId}
 *
 * Tokens are stored at:
 *   devices/{deviceId}/pushTokens/{token}
 */

const functions = require("firebase-functions/v1"); // <-- IMPORTANT (v1 API)
const admin = require("firebase-admin");

admin.initializeApp();

exports.pushOnDeviceNotification = functions.database
  .ref("devices/{deviceId}/notifications/{notifId}")
  .onCreate(async (snap, ctx) => {
    const deviceId = String(ctx.params.deviceId || "");
    const notifId = String(ctx.params.notifId || "");
    const notif = snap.val() || {};

    const title = String(notif.title || "Reef Doser");
    const body = String(notif.body || notif.message || "Device update");
    const severity = String(notif.severity || "info").toLowerCase();

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
        deviceId,
        notifId,
        severity,
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

    await admin
      .database()
      .ref(`devices/${deviceId}/notifications/${notifId}/pushedAt`)
      .set(Date.now());

    return null;
  });