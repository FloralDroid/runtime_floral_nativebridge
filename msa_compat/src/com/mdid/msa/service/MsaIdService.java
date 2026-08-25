package com.mdid.msa.service;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

import com.bun.lib.MsaIdInterface;

/**
 * Package-independent MSA endpoint for environments without a vendor OAID
 * provider. It reports an available interface without fabricating a device ID.
 */
public final class MsaIdService extends Service {
    private final MsaIdInterface.Stub binder = new MsaIdInterface.Stub() {
        @Override
        public boolean isSupported() {
            // The MSA endpoint is available; the empty values represent the
            // absence of a vendor-backed identifier in this container.
            return true;
        }

        @Override
        public boolean isDataArrived() {
            return true;
        }

        @Override
        public String getOAID() {
            return "";
        }

        @Override
        public String getVAID() {
            return "";
        }

        @Override
        public String getAAID() {
            return "";
        }

        @Override
        public void shutDown() {
            stopSelf();
        }
    };

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }
}
