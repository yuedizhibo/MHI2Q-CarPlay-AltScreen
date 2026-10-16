package com.luka.carplay.core;

import com.luka.carplay.bus.CarplayBus;
import com.luka.carplay.framework.Log;
import de.audi.tghu.navi.app.Navigation;

/** Read OEM map state on NavigationJobs; keep the boot latch entirely in RAM.
 *  Until MapKombi is initialized the hook asks the phone for nothing on the
 *  cluster (showUI / forceKeyFrame) and the display does not start; Main110 and
 *  the capability exchange are not delayed.  There is no timeout: without the
 *  OEM map the cluster picture could not be shown anyway. */
public final class OemMapStartupGate {
    private static final int COMMAND = 0x0119;
    private static boolean started;
    private static volatile boolean ready;
    private static volatile boolean pending;
    private static final byte[] READY = new byte[] { 1 };
    private static final Object WAKE = new Object();
    private static final long PROBE_MS = 200L;

    private OemMapStartupGate() { }

    /** Idempotent; called from the boot-time cluster controller start. */
    public static synchronized void start() {
        if (started) return;
        CarplayBus.getInstance().start(); /* idempotent */
        Thread worker = new Thread(new Runnable() {
            public void run() {
                Log.i("OemMapGate", "WAIT source=MapKombi.isInitialized probe_ms=200 ready_wake=IMMEDIATE capabilities=EARLY UI=DEFERRED");
                long nextWaitLog = System.currentTimeMillis() + 60000L;
                while (true) {
                    if (!ready) scheduleProbe();
                    if (!ready && System.currentTimeMillis() >= nextWaitLog) {
                        Log.i("OemMapGate", "WAIT map_not_initialized=1 probe_pending=" + pending);
                        nextWaitLog = System.currentTimeMillis() + 60000L;
                    }
                    boolean observedReady = ready;
                    if (observedReady) announce();
                    synchronized (WAKE) {
                        /* A probe may finish between announce and wait. Do not
                         * lose that notification or delay its first READY. */
                        if (ready == observedReady) {
                            try { WAKE.wait(observedReady ? 1000L : PROBE_MS); }
                            catch (InterruptedException ignored) { }
                        }
                    }
                }
            }
        }, "carplay-oem-map-gate");
        worker.setDaemon(true);
        try {
            worker.start();
            started = true;
        } catch (Throwable failure) {
            Log.e("OemMapGate", "worker start failed; second screen waiting", failure);
        }
    }

    /* Worker thread only. A hook restart drops the bus connection (its socket
     * closes with the process) for longer than one 1 s poll, so every connect
     * seen here starts a new announcement; the capped repeat covers a
     * reconnect that was too quick to see. */
    private static final ReadySchedule schedule = new ReadySchedule();
    private static boolean wasConnected;
    private static int connects;
    /* 1 s worker loop ticks after readiness: a clock-free schedule unit. */
    private static long ticks;

    /** READY on the hook bus: the hook latches it once per process, so it goes
     * out at once on every new bus connection (a restarted hook connects again)
     * and is then repeated with a growing interval (2 s doubling to 60 s) only
     * as a safety net, e.g. for a connection whose first frame was lost,
     * instead of every second for the whole drive. */
    private static void announce() {
        CarplayBus bus = CarplayBus.getInstance();
        boolean connected = bus.isConnected();
        if (connected && !wasConnected) connects++;
        wasConnected = connected;
        if (!connected) return;
        int generation = connects & 0x7fffffff;
        long now = ++ticks;
        if (!schedule.due(generation, now)) return;
        if (bus.send(COMMAND, CarplayBus.FLAG_BINARY, READY, 1)) {
            if (schedule.sent(generation, now))
                Log.i("OemMapGate", "READY sent bus=0119 connection=" + generation);
        }
    }

    /** When READY is due on a bus connection, in worker ticks (1 s); pure, host-tested. */
    static final class ReadySchedule {
        static final long FIRST_REPEAT = 2L, MAX_REPEAT = 60L;
        private int generation = -1;
        private long next, interval;

        boolean due(int connection, long now) {
            return connection != generation || now >= next;
        }

        /** Records a send; true when it was the first on this connection. */
        boolean sent(int connection, long now) {
            boolean first = connection != generation;
            if (first) {
                generation = connection;
                interval = FIRST_REPEAT;
            } else {
                interval = Math.min(MAX_REPEAT, interval * 2L);
            }
            next = now + interval;
            return first;
        }
    }

    private static void scheduleProbe() {
        if (pending) return;
        try {
            final Navigation navigation = Navigation.getInstance();
            if (navigation == null || navigation.getDispatcher() == null) return;
            pending = true;
            navigation.getDispatcher().execute(new Runnable() {
                public void run() {
                    try {
                        if (navigation.getMapManager() != null &&
                            navigation.getMapManager().getMapKombi() != null &&
                            navigation.getMapManager().getMapKombi().isInitialized()) {
                            synchronized (WAKE) {
                                ready = true;
                                WAKE.notifyAll();
                            }
                            Log.i("OemMapGate", "READY source=MapKombi.isInitialized boot_latched=1");
                        }
                    } catch (Throwable ignored) {
                    } finally { pending = false; }
                }
            });
        } catch (Throwable ignored) { pending = false; }
    }
}
