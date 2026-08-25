package com.bun.lib;

/** MSA open anonymous device identifier interface. */
interface MsaIdInterface {
    boolean isSupported();
    boolean isDataArrived();
    String getOAID();
    String getVAID();
    String getAAID();
    void shutDown();
}
