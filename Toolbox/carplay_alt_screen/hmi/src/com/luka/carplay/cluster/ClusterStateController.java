/*
 * Context80 readback controller for the CarPlay Type111 experiment.
 *
 * Vehicle contract:
 *   - Java is the sole terminal1 context writer.
 *   - ctx80={98,101,102,3}.
 *   - BaseVideo demand is active+destination-ready.
 *   - every ctx80 acquisition is confirmed by getCurrentContextID(1)
 *     before compositeApplied becomes true.
 *
 * Platform/HMI calls are deliberately made through reflection so this one
 * class can be rebuilt without vendoring the proprietary lsd.jar. The public
 * ABI remains compatible with the existing unified carplay_hook.jar.
 */
package com.luka.carplay.cluster;

import de.audi.atip.base.IFrameworkAccess;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

public final class ClusterStateController {
    public static final int TERMINAL_CLUSTER = 1;
    public static final int CTX_STOCK = 74;
    public static final int CTX_BOUNCE = 72;
    public static final int CTX_COMPOSITE = 80;

    /* Compatibility seams used by newer geometry-aware unified JARs. */
    public static final int VIEWAREA_FULLSCREEN = 0;
    public static final int VIEWAREA_SMALLSCREEN = 1;

    private static final long POLL_MS = 100L;
    private static final long OEM_PROBE_MS = 500L;
    private static final long OWNERSHIP_PROBE_MS = 500L;
    private static final long OWNERSHIP_HEARTBEAT_MS = 10000L;
    private static final long DISPLAYABLE_STATE_STALE_MS = 3000L;
    private static final long RECONCILE_MS = 250L;
    private static final long BOUNCE_MS = 180L;
    private static final long VERIFY_STEP_MS = 50L;
    private static final int VERIFY_ATTEMPTS = 7;
    private static final long CIRCUIT_BREAKER_MS = 2000L;
    private static final int CONTEXT_FAILURE_LIMIT = 3;
    private static final long DIAG_MAX_BYTES = 131072L;

    private static final String HMI_STATE_FILE = "/tmp/mmi-mirror-hmi.state";
    private static final String CLUSTER_OWNERSHIP_STATE_FILE =
        "/tmp/mmi-mirror-cluster-ownership.state";
    /*
     * OEM_LAYOUT_OBSERVER_V1
     *
     * This snapshot is observation-only.  It deliberately does not drive
     * CarPlay viewAreas/safeArea or the displayable3 renderer until the
     * vehicle-specific ListModel176 values and plane geometry have been
     * correlated on-car.
     */
    private static final String OEM_GEOMETRY_STATE_FILE =
        "/tmp/carplay-oem-geometry.state";
    private static final String OEM_GEOMETRY_HISTORY_FILE =
        "/tmp/carplay-oem-geometry.log";
    private static final String OEM_DISPLAYMANAGER_API_FILE =
        "/tmp/carplay-oem-displaymanager-read-api.log";
    private static final long OEM_HISTORY_MAX_BYTES = 262144L;
    private static final int OEM_SCREEN_LAYOUT_MODEL_ID = 176;
    private static final int OEM_SCREEN_LAYOUT_ROW = 1;
    private static final int OEM_REQUIRED_COLUMN_COUNT = 28;
    private static final int OEM_MISSING = Integer.MIN_VALUE;
    private static final String BASEVIDEO_ACTIVE_FILE = "/tmp/mmi-altscreen-active";
    private static final String BASEVIDEO_READY_FILE = "/tmp/mmi-altscreen-basevideo.ready";
    private static final String DISPLAYABLE3_STATE_FILE =
        "/tmp/mmi-altscreen-displayable3.state";
    private static final String CONTEXT_MODE_FILE = "/tmp/mmi-mirror-context.mode";
    private static final String STARTED_FILE = "/tmp/mmi-altscreen-controller.started";
    private static final String DIAG_FILE = "/tmp/mmi-mirror-controller.log";
    private static final String MODE_JAVA80 = "JAVA80";

    private static final Object LOCK = new Object();
    private static final Object DIAG_LOCK = new Object();
    /* Cache method metadata, never HMI objects or values: those can change
     * when the firmware replaces a layout/model during a context switch.
     * Bound retained classes for a resident JVM with reloaded services. */
    private static final Object METHOD_LOCK = new Object();
    private static final int METHOD_CACHE_LIMIT = 64;
    private static final Class[] NO_PARAMETERS = new Class[0];
    private static final Class[] INT_PARAMETER = new Class[]{Integer.TYPE};
    private static final Object[] NO_ARGUMENTS = new Object[0];
    private static final Class[] methodClasses = new Class[METHOD_CACHE_LIMIT];
    private static final String[] methodNames = new String[METHOD_CACHE_LIMIT];
    private static final boolean[] methodIntParameters = new boolean[METHOD_CACHE_LIMIT];
    private static final Method[] cachedMethods = new Method[METHOD_CACHE_LIMIT];
    private static int methodCacheSize;
    private static int methodCacheNext;

    private static volatile IFrameworkAccess frameworkAccess;
    private static volatile Thread worker;
    private static volatile Thread contextWriterThread;
    private static volatile boolean ownershipIntent;
    private static volatile boolean compositeApplied;
    private static volatile boolean carPlaySessionActive;
    private static volatile boolean rgiPresentationActive;
    private static volatile int rgiWindowRevision;
    private static volatile int rgiConfirmedRevision;
    private static volatile boolean smallScreenViewArea;

    private static String lastStateSignature = "";
    private static String lastContextMode = "";
    private static String lastObserverStatus = "";
    private static String lastOemGeometrySignature = "";
    private static String lastOemProbeStatus = "";
    private static String lastOemModelAccess = "UNRESOLVED";
    private static String lastOwnershipSignature = "";
    private static String lastClusterOwnershipSignature = "";
    private static long oemGeometryRevision;
    private static long lastOemProbeMs;
    private static long lastOwnershipProbeMs;
    private static long lastOwnershipHeartbeatMs;
    private static long lastReconcileMs;
    private static boolean displayManagerApiProbed;
    private static int contextWriteFailures;
    private static long circuitOpenUntilMs;
    private static int navViewSizeChoiceId = Integer.MIN_VALUE;
    private static boolean navViewSizeChoiceResolved;
    private static boolean geometryClassMissing;

    private ClusterStateController() {}

    public static void start(IFrameworkAccess fw) {
        if (fw != null) frameworkAccess = fw;
        synchronized (LOCK) {
            if (worker != null && worker.isAlive()) {
                LOCK.notifyAll();
                return;
            }
            try {
                writeStartedMarker();
                diag("controller start requested; frameworkAccess="
                    + (frameworkAccess != null ? "ok" : "null")
                    + " ctx_readback=getCurrentContextID(1)");
                Thread t = new Thread(new Runnable() {
                    public void run() { runLoop(); }
                }, "cluster-state-controller");
                t.setDaemon(true);
                t.start();
                worker = t;
            } catch (Throwable t) {
                diag("ERROR worker start failed: " + describe(t));
            }
        }
    }

    public static void setCarPlaySessionActive(boolean active) {
        if (carPlaySessionActive == active) return;
        carPlaySessionActive = active;
        lastStateSignature = "";
        lastClusterOwnershipSignature = "";
        diag("carplay_session=" + (active ? "1" : "0"));
        WheelZoomBridge.logCarPlayLifecycle(active);
        if (!active) WheelZoomBridge.reset();
    }

    public static void setRgiPresentationActive(boolean active) {
        if (rgiPresentationActive == active) return;
        rgiPresentationActive = active;
        lastStateSignature = "";
        diag("rgi_active=" + (active ? "1" : "0"));
        geometryReapply("rgi-presentation-change");
    }

    /* Called only after READY + FRAME_READY. A new window needs a real
     * context transition even if the map already owns 80. The worker remains
     * the sole terminal-1 context writer; RGI never invokes dmdt. */
    public static String activateRgiRenderer() {
        final int revision;
        synchronized (LOCK) {
            revision = ++rgiWindowRevision;
            setRgiPresentationActive(true);
        }
        long deadline = nowMs() + 3000L;
        while (nowMs() < deadline) {
            if (!rgiPresentationActive || revision != rgiWindowRevision)
                return "FAILED: RGI activation withdrawn";
            if (rgiConfirmedRevision == revision && compositeApplied)
                return "cluster ctx80 RGI98 verified revision=" + revision;
            sleep(25L);
        }
        synchronized (LOCK) {
            if (revision == rgiWindowRevision) setRgiPresentationActive(false);
        }
        return "FAILED: RGI98 ctx80 readback timeout";
    }

    public static void deactivateRgiRenderer() {
        synchronized (LOCK) {
            ++rgiWindowRevision;
            setRgiPresentationActive(false);
        }
    }

    public static boolean isRgiPresentationActive() {
        return rgiPresentationActive;
    }

    /** Queue a new window binding on the sole context writer, without blocking RGI I/O. */
    public static void requestRgiRendererRebind() {
        synchronized (LOCK) {
            if (!rgiPresentationActive) return;
            ++rgiWindowRevision;
            LOCK.notifyAll();
        }
    }

    public static boolean isCarPlaySessionActive() { return carPlaySessionActive; }

    public static boolean isClusterOwned() {
        return carPlaySessionActive && ownershipIntent && compositeApplied;
    }

    public static boolean isContextWriterThread() {
        return Thread.currentThread() == contextWriterThread;
    }

    /* Kept for older DisplayManager guards. Absent/unknown mode defaults JAVA80. */
    public static boolean isCompositeModeRequested() {
        return MODE_JAVA80.equals(readContextMode());
    }

    public static boolean isSmallScreenViewArea() {
        return smallScreenViewArea;
    }

    public static void setViewAreaMode(int mode) {
        boolean small = mode == VIEWAREA_SMALLSCREEN;
        if (smallScreenViewArea == small) return;
        smallScreenViewArea = small;
        com.luka.carplay.core.ScreenModule.onViewAreaModeChanged(mode);
        geometryReapply("view-area-change");
    }

    private static void runLoop() {
        contextWriterThread = Thread.currentThread();
        diag("worker running; writerThread=" + contextWriterThread.getName());
        while (true) {
            try {
                /*
                 * Keep the proven V2 display/control path higher priority than
                 * the diagnostic OEM observer.  If a proprietary HMI model
                 * lookup is slow or unavailable, it must never delay the first
                 * ctx80 acquisition/reconcile.
                 */
                pollContextPolicy();
                publishClusterOwnershipState();
                pollOwnershipDiagnostics();
                pollHmiState();
            } catch (Throwable t) {
                diag("ERROR poll failed: " + describe(t));
            }
            sleep(POLL_MS);
        }
    }

    /*
     * COLD_START_OWNERSHIP_DIAG_V1
     *
     * Observation only. Correlates terminal1 Context80 with the sidecar's
     * /tmp-only displayable3 snapshot. It does not gate Private111, switch an
     * additional context, enumerate Screen windows, or rebind displayable3.
     */
    private static void pollOwnershipDiagnostics() {
        long now = nowMs();
        if (lastOwnershipProbeMs != 0L
            && now - lastOwnershipProbeMs < OWNERSHIP_PROBE_MS)
            return;
        lastOwnershipProbeMs = now;

        boolean baseActive = new File(BASEVIDEO_ACTIVE_FILE).exists();
        boolean baseReady = new File(BASEVIDEO_READY_FILE).exists();
        boolean displayStatePresent = new File(DISPLAYABLE3_STATE_FILE).exists();
        if (!carPlaySessionActive && !rgiPresentationActive
            && !baseReady && !displayStatePresent
            && !ownershipIntent && !compositeApplied)
            return;

        Object dm = displayManager();
        int actual = currentContext(dm);
        String state = readSmallState(DISPLAYABLE3_STATE_FILE, 4096);
        boolean displayStateReadable = state.length() != 0;
        long stateTs = stateLong(state, "timestamp_ms", -1L);
        long stateAge = stateTs >= 0L && now >= stateTs ? now - stateTs : -1L;
        int backendReady = (int)stateLong(state, "backend_ready", -1L);
        int nativePresent = (int)stateLong(state, "native_window_present", -1L);
        int visibleValid = (int)stateLong(state, "visible_valid", -1L);
        int visible = (int)stateLong(state, "visible", -1L);
        int firstPresent = (int)stateLong(state, "first_present", -1L);
        long presented = stateLong(state, "presented_frames", -1L);
        long generation = stateLong(state, "generation", -1L);
        long sequence = stateLong(state, "sequence", -1L);
        long h264Packets = stateLong(state, "h264_packets", -1L);
        long decodedFrames = stateLong(state, "decoded_frames", -1L);
        String nativeWindow = stateValue(state, "native_window", "?");
        String kdWindow = stateValue(state, "kd_window", "?");
        String manager = stateValue(state, "manager", "?");
        boolean stale = state.length() == 0
            || stateAge < 0L || stateAge > DISPLAYABLE_STATE_STALE_MS;

        String signature =
            actual + "/" + (ownershipIntent ? "1" : "0")
            + "/" + (compositeApplied ? "1" : "0")
            + "/" + (carPlaySessionActive ? "1" : "0")
            + "/" + (rgiPresentationActive ? "1" : "0")
            + "/" + (baseActive ? "1" : "0")
            + "/" + (baseReady ? "1" : "0")
            + "/" + (displayStatePresent ? "1" : "0")
            + "/" + (displayStateReadable ? "1" : "0")
            + "/" + backendReady + "/" + nativePresent
            + "/" + visibleValid + "/" + visible
            + "/" + firstPresent + "/" + nativeWindow
            + "/" + kdWindow + "/" + manager
            + "/" + generation
            + "/" + oemGeometryRevision
            + "/" + lastOemProbeStatus;

        boolean changed = !signature.equals(lastOwnershipSignature);
        boolean heartbeat = lastOwnershipHeartbeatMs == 0L
            || now - lastOwnershipHeartbeatMs >= OWNERSHIP_HEARTBEAT_MS;
        if (!changed && !heartbeat) return;

        if (changed) lastOwnershipSignature = signature;
        if (heartbeat) lastOwnershipHeartbeatMs = now;

        diag("OWNERSHIP_SNAPSHOT reason=" + (changed ? "change" : "heartbeat")
            + " ctx=" + actual + " desired=80"
            + " ownership=" + (ownershipIntent ? "1" : "0")
            + " composite=" + (compositeApplied ? "1" : "0")
            + " cp=" + (carPlaySessionActive ? "1" : "0")
            + " rgi=" + (rgiPresentationActive ? "1" : "0")
            + " base=" + (baseActive ? "1" : "0")
            + "/" + (baseReady ? "1" : "0")
            + " display_state_present=" + (displayStatePresent ? "1" : "0")
            + " display_state_readable=" + (displayStateReadable ? "1" : "0")
            + " display_state_age_ms=" + stateAge
            + " display_state_stale=" + (stale ? "1" : "0")
            + " backend_ready=" + backendReady
            + " native_present=" + nativePresent
            + " native=" + nativeWindow
            + " kd=" + kdWindow
            + " visible_valid=" + visibleValid
            + " visible=" + visible
            + " first_present=" + firstPresent
            + " presented=" + presented
            + " gen=" + generation
            + " seq=" + sequence
            + " h264_packets=" + h264Packets
            + " decoded_frames=" + decodedFrames
            + " manager=" + sanitizeStateValue(manager)
            + " oem_rev=" + oemGeometryRevision
            + " oem_status=" + sanitizeStateValue(lastOemProbeStatus)
            + " observe_only=1");

        if (changed && compositeApplied && actual >= 0
            && actual != CTX_COMPOSITE) {
            diag("OWNERSHIP_SUSPECT kind=CONTEXT_DRIFT"
                + " actual=" + actual + " desired=80"
                + " display_visible=" + visible
                + " gen=" + generation + " seq=" + sequence);
        }
        if (changed && compositeApplied && actual == CTX_COMPOSITE
            && visibleValid == 1 && visible == 0) {
            diag("OWNERSHIP_SUSPECT kind=CTX80_WITH_DISPLAYABLE_HIDDEN"
                + " actual=80 display_visible=0"
                + " native=" + nativeWindow + " kd=" + kdWindow
                + " gen=" + generation + " seq=" + sequence);
        }
        if (changed && compositeApplied && stale) {
            diag("OWNERSHIP_SUSPECT kind=DISPLAYABLE_STATE_STALE"
                + " actual=" + actual
                + " state_age_ms=" + stateAge
                + " present=" + (displayStatePresent ? "1" : "0")
                + " readable=" + (displayStateReadable ? "1" : "0")
                + " base=" + (baseActive ? "1" : "0")
                + "/" + (baseReady ? "1" : "0"));
        }
    }

    private static void pollHmiState() {
        Object fw = frameworkAccess;
        if (fw == null) {
            observerStatus("frameworkAccess=null");
            return;
        }

        Object hmi;
        try {
            hmi = invokeNoArg(fw, "getHMIService");
        } catch (Throwable t) {
            observerStatus("getHMIService failed: " + describe(t));
            return;
        }
        if (hmi == null) {
            observerStatus("HMIService=null");
            return;
        }

        int choiceId = resolveNavViewSizeChoiceId();
        if (choiceId < 0) {
            observerStatus("NAV_VIEW_SIZE_CHOICE id unresolved");
            return;
        }

        boolean small;
        String choiceClass;
        int choiceValue;
        try {
            Object model = invokeInt(hmi, "getModel", choiceId);
            if (model == null) {
                observerStatus("NAV_VIEW_SIZE_CHOICE model=null");
                return;
            }
            choiceClass = model.getClass().getName();
            Object value = invokeNoArg(model, "getValue");
            if (!(value instanceof Integer)) {
                observerStatus("NAV_VIEW_SIZE_CHOICE value non-int class=" + choiceClass);
                return;
            }
            choiceValue = ((Integer)value).intValue();
            small = choiceValue == 1;
            setViewAreaMode(small ? VIEWAREA_SMALLSCREEN : VIEWAREA_FULLSCREEN);
        } catch (Throwable t) {
            observerStatus("NAV_VIEW_SIZE_CHOICE read failed: " + describe(t));
            return;
        }

        String layoutName = "unknown";
        Object layoutObject = null;
        int smallDx = 0;
        int smallDy = 0;
        try {
            Object terminal = invokeInt(hmi, "getHMITerminal", TERMINAL_CLUSTER);
            if (terminal == null) {
                observerStatus("getHMITerminal(1)=null; choice=" + choiceValue
                    + " class=" + choiceClass);
                return;
            }
            layoutObject = invokeNoArg(terminal, "getLayout");
            if (layoutObject == null) {
                observerStatus("terminal1 layout=null; choice=" + choiceValue
                    + " class=" + choiceClass);
                return;
            }
            layoutName = layoutObject.getClass().getName();
            smallDx = readLayoutConstant(layoutObject, 80);
            smallDy = readLayoutConstant(layoutObject, 81);
        } catch (Throwable t) {
            observerStatus("terminal/layout read failed: " + describe(t)
                + " choice=" + choiceValue + " class=" + choiceClass);
            return;
        }

        String lower = layoutName.toLowerCase();
        boolean sport = lower.indexOf("sport") >= 0 || smallDx != 0 || smallDy != 0;
        String layout = sport ? "SPORT" : "CLASSIC";
        String view = small ? "SMALL" : "FULL";

        /*
         * The legacy CLASSIC/SPORT hint above is retained only for the old
         * mmi-mirror-hmi.state compatibility file.  K1004 reverse engineering
         * proved that the actual OEM geometry comes from ListModel 176 row 1;
         * do not use the class-name hint as a geometry source.
         */
        /*
         * Observation starts only after the proven Java80 composite has been
         * physically verified.  This keeps all unproven ListModel176/reflection
         * work out of the startup-critical path.
         */
        if (compositeApplied) {
            pollOemGeometry(hmi, layoutObject, choiceValue, layoutName, view);
        }

        observerStatus("ok choice=" + choiceValue + " choiceClass=" + choiceClass
            + " layoutClass=" + layoutName + " c80=" + smallDx + " c81=" + smallDy
            + " -> " + layout + "_" + view);

        String signature = layout + "/" + view + "/" + layoutName + "/"
            + smallDx + "/" + smallDy
            + "/cp=" + (carPlaySessionActive ? "1" : "0")
            + "/owned=" + (isClusterOwned() ? "1" : "0")
            + "/rgi=" + (rgiPresentationActive ? "1" : "0");
        if (!signature.equals(lastStateSignature)) {
            lastStateSignature = signature;
            if (writeHmiState(layout, view, layoutName, smallDx, smallDy)) {
                diag("state published: " + layout + "_" + view
                    + " layoutClass=" + layoutName
                    + " c80=" + smallDx + " c81=" + smallDy);
            }
        }
    }

    /*
     * Publish the V3 wheel-control ownership gate independently of the OEM
     * layout observer.  This path depends only on the proven Java80 context
     * lifecycle and an explicit getCurrentContextID(1) readback, so a missing
     * NAV_VIEW_SIZE_CHOICE/ListModel176 can never disable wheel control.
     */
    private static void publishClusterOwnershipState() {
        boolean cp = carPlaySessionActive;
        boolean intent = ownershipIntent;
        boolean applied = compositeApplied;
        int actual = -1;
        if (cp && intent && applied) {
            Object dm = displayManager();
            actual = currentContext(dm);
        }
        boolean owned = cp && intent && applied && actual == CTX_COMPOSITE;
        String signature = (cp ? "1" : "0")
            + "/" + (intent ? "1" : "0")
            + "/" + (applied ? "1" : "0")
            + "/" + actual
            + "/" + (owned ? "1" : "0");
        if (signature.equals(lastClusterOwnershipSignature)
            && new File(CLUSTER_OWNERSHIP_STATE_FILE).exists()) return;

        String text = "version=1\n"
            + "carplay_session=" + (cp ? "1" : "0") + "\n"
            + "cluster_owned=" + (owned ? "1" : "0") + "\n"
            + "ownership_intent=" + (intent ? "1" : "0") + "\n"
            + "composite_applied=" + (applied ? "1" : "0") + "\n"
            + "context=" + actual + "\n"
            + "timestamp_ms=" + nowMs() + "\n";
        if (writeAtomicState(CLUSTER_OWNERSHIP_STATE_FILE, text)) {
            lastClusterOwnershipSignature = signature;
            diag("cluster ownership published cp=" + (cp ? "1" : "0")
                + " intent=" + (intent ? "1" : "0")
                + " composite=" + (applied ? "1" : "0")
                + " actual=" + actual
                + " owned=" + (owned ? "1" : "0"));
        }
    }

    private static void pollContextPolicy() {
        String mode = readContextMode();
        if (!mode.equals(lastContextMode)) {
            diag("context mode " + lastContextMode + " -> " + mode);
            lastContextMode = mode;
        }
        if (!MODE_JAVA80.equals(mode)) return;

        boolean baseActive = new File(BASEVIDEO_ACTIVE_FILE).exists();
        boolean baseReady = new File(BASEVIDEO_READY_FILE).exists();
        boolean wantComposite = (baseActive && baseReady) || rgiPresentationActive;

        if (!wantComposite) {
            if (ownershipIntent || compositeApplied) {
                ownershipIntent = false;
                Object dm = displayManager();
                boolean ok = dm != null && selectContext(dm, CTX_STOCK, "release");
                compositeApplied = false;
                contextWriteFailures = 0;
                if (ok) {
                    if (!verifyContext(dm, CTX_STOCK, "release")) {
                        diag("CTX74_VERIFY_WARN desired=74");
                    }
                    geometryReapply("ctx74-release");
                    diag("ownership released -> ctx74");
                }
            }
            return;
        }

        long now = nowMs();
        if (now < circuitOpenUntilMs) return;

        ownershipIntent = true;
        Object dm = displayManager();
        if (dm == null) {
            contextFailure("DisplayManager unavailable");
            return;
        }

        int rgiRevision = rgiWindowRevision;
        boolean rebindRgi = rgiPresentationActive
            && rgiConfirmedRevision != rgiRevision;
        if (!compositeApplied || rebindRgi) {
            int actual = currentContext(dm);
            diag("ownership acquire requested; base=" + (baseActive ? "1" : "0")
                + "/" + (baseReady ? "1" : "0")
                + " rgi=" + (rgiPresentationActive ? "1" : "0")
                + " actual=" + actual);

            String verifyReason = "already-active";
            if (actual != CTX_COMPOSITE || rebindRgi) {
                if (!selectContext(dm, CTX_BOUNCE, "enter-bounce")) {
                    contextFailure("ctx72 bounce failed");
                    return;
                }
                sleep(BOUNCE_MS);
                if (!ownershipIntent) return;
                if (!selectContext(dm, CTX_COMPOSITE, "enter-composite")) {
                    contextFailure("ctx80 enter failed");
                    return;
                }
                verifyReason = "enter-composite";
            }

            if (!verifyContext(dm, CTX_COMPOSITE, verifyReason)) {
                contextFailure("ctx80 readback verification failed");
                return;
            }

            compositeApplied = true;
            contextWriteFailures = 0;
            lastReconcileMs = nowMs();
            geometryReapply("ctx80-acquired");
            if (rgiPresentationActive && rgiWindowRevision == rgiRevision)
                rgiConfirmedRevision = rgiRevision;
            diag("ownership acquired -> ctx80 verified=1");
            return;
        }

        now = nowMs();
        if (now - lastReconcileMs < RECONCILE_MS) return;
        lastReconcileMs = now;

        int actual = currentContext(dm);
        if (actual >= 0 && actual != CTX_COMPOSITE) {
            diag("physical context drift actual=" + actual
                + " desired=80 -> Java reconcile");
            if (!selectContext(dm, CTX_COMPOSITE, "reconcile")) {
                contextFailure("ctx80 reconcile failed");
                return;
            }
            if (!verifyContext(dm, CTX_COMPOSITE, "reconcile")) {
                contextFailure("ctx80 reconcile readback failed");
                return;
            }
            contextWriteFailures = 0;
            geometryReapply("ctx80-reconcile");
        }
    }

    private static Object displayManager() {
        try {
            Object fw = frameworkAccess;
            if (fw == null) return null;
            Object hmi = invokeNoArg(fw, "getHMIService");
            if (hmi == null) return null;
            return invokeNoArg(hmi, "getDisplayManager");
        } catch (Throwable t) {
            diag("DisplayManager lookup failed: " + describe(t));
            return null;
        }
    }

    private static int currentContext(Object dm) {
        if (dm == null) return -1;
        try {
            Method m = cachedMethod(dm.getClass(), "getCurrentContextID", true);
            Object value = m.invoke(dm, new Object[]{new Integer(TERMINAL_CLUSTER)});
            if (!(value instanceof Integer)) {
                diag("currentContext read returned non-int");
                return -1;
            }
            return ((Integer)value).intValue();
        } catch (Throwable t) {
            diag("currentContext read failed: " + describe(t));
            return -1;
        }
    }

    private static boolean selectContext(Object dm, int context, String reason) {
        if (dm == null) return false;
        try {
            Method[] methods = dm.getClass().getMethods();
            Method target = null;
            int i;
            for (i = 0; i < methods.length; ++i) {
                Method m = methods[i];
                if (!"switchContext".equals(m.getName())) continue;
                Class[] p = m.getParameterTypes();
                if (p.length == 3 && p[0] == Integer.TYPE && p[1] == Integer.TYPE) {
                    target = m;
                    break;
                }
            }
            if (target == null) {
                diag("ERROR switchContext(int,int,*) method not found");
                return false;
            }
            diag(reason + " -> ctx" + context);
            target.invoke(dm, new Object[]{
                new Integer(context),
                new Integer(TERMINAL_CLUSTER),
                null
            });
            return true;
        } catch (Throwable t) {
            diag("ERROR switch ctx" + context + " failed: " + describe(t));
            return false;
        }
    }

    private static boolean verifyContext(Object dm, int desired, String reason) {
        int actual = -1;
        int attempt;
        for (attempt = 1; attempt <= VERIFY_ATTEMPTS; ++attempt) {
            actual = currentContext(dm);
            if (actual == desired) {
                if (desired == CTX_COMPOSITE) {
                    diag("CTX80_OBSERVED actual=80 desired=80 reason=" + reason
                        + " attempts=" + attempt
                        + " source=IDisplayManager.getCurrentContextID");
                } else {
                    diag("CTX_OBSERVED actual=" + actual + " desired=" + desired
                        + " reason=" + reason + " attempts=" + attempt);
                }
                return true;
            }
            if (attempt < VERIFY_ATTEMPTS) sleep(VERIFY_STEP_MS);
        }
        if (desired == CTX_COMPOSITE) {
            diag("CTX80_VERIFY_FAIL actual=" + actual + " desired=80 reason=" + reason
                + " attempts=" + VERIFY_ATTEMPTS);
        } else {
            diag("CTX_VERIFY_FAIL actual=" + actual + " desired=" + desired
                + " reason=" + reason + " attempts=" + VERIFY_ATTEMPTS);
        }
        return false;
    }

    private static void contextFailure(String reason) {
        ++contextWriteFailures;
        diag("context failure " + contextWriteFailures + "/"
            + CONTEXT_FAILURE_LIMIT + ": " + reason);
        if (contextWriteFailures < CONTEXT_FAILURE_LIMIT) return;
        ownershipIntent = false;
        compositeApplied = false;
        circuitOpenUntilMs = nowMs() + CIRCUIT_BREAKER_MS;
        contextWriteFailures = 0;
        diag("context circuit breaker OPEN for " + CIRCUIT_BREAKER_MS
            + " ms; ownership released");
    }


    /*
     * Read-only K1004 OEM geometry probe.
     *
     * Reverse-engineering evidence:
     *   ListModel 176 row 1:
     *     0 screenWidth, 1 screenHeight,
     *     8 infolineBottom, 9 reiterlineTop,
     *     11/12 full tube L/R, 13/14 small(KB) tube L/R,
     *     24/25 map offsets, 26/27 map H/W.
     *
     * getVisibleArea:
     *   x = tubeLeft - mapOffsetLeft
     *   y = reiterlineTop - mapOffsetTop
     *   w = screenWidth - tubeLeft - tubeRight
     *   h = screenHeight - reiterlineTop - infolineBottom
     *
     * Nothing in this method changes a HMI model, context, displayable,
     * CarPlay dictionary, decoder or renderer.
     */
    private static void pollOemGeometry(Object hmi, Object layoutObject,
                                        int choiceValue, String layoutName,
                                        String view) {
        long now = nowMs();
        if (now - lastOemProbeMs < OEM_PROBE_MS) return;
        lastOemProbeMs = now;

        probeDisplayManagerReadApi();

        try {
            Object list = resolveListModel176(hmi);
            if (list == null) {
                oemProbeUnavailable("ListModel176 unavailable access="
                + lastOemModelAccess);
                return;
            }

            Object lengthValue = invokeNoArg(list, "getLength");
            int rowCount = numberValue(lengthValue, -1);
            if (rowCount <= OEM_SCREEN_LAYOUT_ROW) {
                oemProbeUnavailable("ListModel176 length=" + rowCount);
                return;
            }

            Object row = invokeInt(list, "getRow", OEM_SCREEN_LAYOUT_ROW);
            if (row == null) {
                oemProbeUnavailable("ListModel176 row1=null");
                return;
            }

            int colCount = numberValue(invokeNoArg(row, "getColumnCount"), -1);
            if (colCount < OEM_REQUIRED_COLUMN_COUNT) {
                oemProbeUnavailable("ListModel176 row1 columns=" + colCount
                    + " required>=" + OEM_REQUIRED_COLUMN_COUNT);
                return;
            }

            int[] values = readIntegerRow(row, colCount);
            if (!oemColumnsValid(values)) {
                oemProbeUnavailable("ListModel176 required integer columns missing");
                return;
            }

            int screenWidth = values[0];
            int screenHeight = values[1];
            int infolineBottom = values[8];
            int reiterlineTop = values[9];
            int tubeLeft = values[11];
            int tubeRight = values[12];
            int tubeLeftKb = values[13];
            int tubeRightKb = values[14];
            int mapOffsetLeft = values[24];
            int mapOffsetTop = values[25];
            int mapHeightRaw = values[26];
            int mapWidthRaw = values[27];

            int mapWidth = mapWidthRaw > 0 ? mapWidthRaw : screenWidth;
            int mapHeight = mapHeightRaw > 0 ? mapHeightRaw : screenHeight;

            int fullX = tubeLeft - mapOffsetLeft;
            int fullY = reiterlineTop - mapOffsetTop;
            int fullW = screenWidth - tubeLeft - tubeRight;
            int fullH = screenHeight - reiterlineTop - infolineBottom;

            int smallX = tubeLeftKb - mapOffsetLeft;
            int smallY = fullY;
            int smallW = screenWidth - tubeLeftKb - tubeRightKb;
            int smallH = fullH;

            if (!oemGeometrySane(screenWidth, screenHeight,
                                 mapWidth, mapHeight,
                                 fullW, fullH, smallW, smallH)) {
                oemProbeUnavailable("ListModel176 geometry failed sanity check"
                    + " screen=" + screenWidth + "x" + screenHeight
                    + " map=" + mapWidth + "x" + mapHeight
                    + " full=" + fullW + "x" + fullH
                    + " small=" + smallW + "x" + smallH
                    + " access=" + lastOemModelAccess);
                return;
            }

            boolean small = choiceValue == 1;
            int activeX = small ? smallX : fullX;
            int activeY = small ? smallY : fullY;
            int activeW = small ? smallW : fullW;
            int activeH = small ? smallH : fullH;

            int c80 = readLayoutConstantSafe(layoutObject, 80);
            int c81 = readLayoutConstantSafe(layoutObject, 81);
            int c108 = readLayoutConstantSafe(layoutObject, 108);
            int c109 = readLayoutConstantSafe(layoutObject, 109);
            int c114 = readLayoutConstantSafe(layoutObject, 114);
            int c115 = readLayoutConstantSafe(layoutObject, 115);

            String rowValues = serializeIntegerRow(values);
            String signature = choiceValue + "/" + layoutName + "/"
                + rowValues + "/" + c80 + "/" + c81 + "/" + c108 + "/"
                + c109 + "/" + c114 + "/" + c115
                + "/access=" + lastOemModelAccess
                + "/cp=" + (carPlaySessionActive ? "1" : "0")
                + "/rgi=" + (rgiPresentationActive ? "1" : "0");

            if (!signature.equals(lastOemGeometrySignature)) {
                long nextRevision = oemGeometryRevision + 1L;
                String layoutHint = layoutName.toLowerCase().indexOf("sport") >= 0
                    ? "SPORT_HINT" : "UNKNOWN";
                String text = "schema=1\n"
                    + "observer=OEM_LAYOUT_OBSERVER_V1\n"
                    + "mode=OBSERVE_ONLY\n"
                    + "valid=1\n"
                    + "revision=" + nextRevision + "\n"
                    + "timestamp_ms=" + now + "\n"
                    + "apply_to_carplay=0\n"
                    + "apply_to_renderer=0\n"
                    + "nav_view_size_choice=" + choiceValue + "\n"
                    + "view=" + view + "\n"
                    + "layout_class=" + layoutName + "\n"
                    + "layout_hint=" + layoutHint + "\n"
                    + "list_model_id=176\n"
                    + "list_model_access=" + lastOemModelAccess + "\n"
                    + "list_model_class=" + list.getClass().getName() + "\n"
                    + "list_row=1\n"
                    + "list_length=" + rowCount + "\n"
                    + "row_class=" + row.getClass().getName() + "\n"
                    + "row1_column_count=" + colCount + "\n"
                    + "row1_values=" + rowValues + "\n"
                    + "screen_width=" + screenWidth + "\n"
                    + "screen_height=" + screenHeight + "\n"
                    + "infoline_bottom=" + infolineBottom + "\n"
                    + "reiterline_top=" + reiterlineTop + "\n"
                    + "tube_left_full=" + tubeLeft + "\n"
                    + "tube_right_full=" + tubeRight + "\n"
                    + "tube_left_small=" + tubeLeftKb + "\n"
                    + "tube_right_small=" + tubeRightKb + "\n"
                    + "map_offset_left=" + mapOffsetLeft + "\n"
                    + "map_offset_top=" + mapOffsetTop + "\n"
                    + "map_width_raw=" + mapWidthRaw + "\n"
                    + "map_height_raw=" + mapHeightRaw + "\n"
                    + "map_width_effective=" + mapWidth + "\n"
                    + "map_height_effective=" + mapHeight + "\n"
                    + "visible_full_x=" + fullX + "\n"
                    + "visible_full_y=" + fullY + "\n"
                    + "visible_full_w=" + fullW + "\n"
                    + "visible_full_h=" + fullH + "\n"
                    + "visible_small_x=" + smallX + "\n"
                    + "visible_small_y=" + smallY + "\n"
                    + "visible_small_w=" + smallW + "\n"
                    + "visible_small_h=" + smallH + "\n"
                    + "visible_active_x=" + activeX + "\n"
                    + "visible_active_y=" + activeY + "\n"
                    + "visible_active_w=" + activeW + "\n"
                    + "visible_active_h=" + activeH + "\n"
                    + "layout_const_80=" + c80 + "\n"
                    + "layout_const_81=" + c81 + "\n"
                    + "layout_const_108=" + c108 + "\n"
                    + "layout_const_109=" + c109 + "\n"
                    + "layout_const_114=" + c114 + "\n"
                    + "layout_const_115=" + c115 + "\n"
                    + "carplay_session=" + (carPlaySessionActive ? "1" : "0") + "\n"
                    + "rgi_active=" + (rgiPresentationActive ? "1" : "0") + "\n";

                if (writeAtomicState(OEM_GEOMETRY_STATE_FILE, text)) {
                    /*
                     * Commit the signature/revision only after the state file
                     * was successfully published.  A transient /tmp write
                     * failure must be retried on the next poll rather than
                     * suppressing this geometry forever.
                     */
                    oemGeometryRevision = nextRevision;
                    lastOemGeometrySignature = signature;
                    appendOemGeometryHistory(text);
                    lastOemProbeStatus = "valid";
                    diag("OEM_GEOMETRY_OBSERVER publish revision="
                        + oemGeometryRevision + " view=" + view
                        + " active=" + activeX + "," + activeY + ","
                        + activeW + "x" + activeH
                        + " screen=" + screenWidth + "x" + screenHeight
                        + " map=" + mapWidth + "x" + mapHeight
                        + " apply=NONE");
                }
            }
        } catch (Throwable t) {
            oemProbeUnavailable(describe(t));
        }
    }


    /*
     * Record every geometry signature change so one vehicle session can
     * capture FULL/SMALL and any skin/layout transitions without manually
     * copying the state file after each switch.
     */
    private static void appendOemGeometryHistory(String snapshot) {
        FileOutputStream out = null;
        try {
            File f = new File(OEM_GEOMETRY_HISTORY_FILE);
            boolean reset = f.exists() && f.length() > OEM_HISTORY_MAX_BYTES;
            out = new FileOutputStream(f, !reset);
            if (reset) {
                out.write(("--- history reset at " + nowMs() + " ---\n")
                    .getBytes("UTF-8"));
            }
            String header = "--- OEM_GEOMETRY_SNAPSHOT revision="
                + oemGeometryRevision + " time_ms=" + nowMs() + " ---\n";
            out.write(header.getBytes("UTF-8"));
            out.write(snapshot.getBytes("UTF-8"));
            out.write("--- END_OEM_GEOMETRY_SNAPSHOT ---\n".getBytes("UTF-8"));
            out.flush();
            out.close();
            out = null;
        } catch (Throwable t) {
            diag("WARN OEM geometry history write failed: " + describe(t));
            try { if (out != null) out.close(); } catch (Throwable ignored) {}
        }
    }

    /*
     * The reverse report could not prove a Java getter for displayable33/58
     * extents.  Do not guess or invoke unknown methods.  Instead, capture the
     * runtime DisplayManager class and the signatures of read-looking APIs
     * once.  This is reflection metadata only; no method below is invoked.
     */
    private static void probeDisplayManagerReadApi() {
        if (displayManagerApiProbed) return;
        try {
            Object dm = displayManager();
            if (dm == null) return;

            Method[] methods = dm.getClass().getMethods();
            StringBuffer text = new StringBuffer();
            text.append("observer=OEM_LAYOUT_OBSERVER_V1\n");
            text.append("mode=REFLECTION_METADATA_ONLY\n");
            text.append("display_manager_class=")
                .append(dm.getClass().getName()).append('\n');

            int hits = 0;
            int i;
            for (i = 0; i < methods.length; ++i) {
                Method m = methods[i];
                String name = m.getName();
                String lower = name.toLowerCase();
                if (lower.indexOf("displayable") < 0
                    && lower.indexOf("extent") < 0
                    && lower.indexOf("position") < 0
                    && lower.indexOf("size") < 0
                    && lower.indexOf("source") < 0) {
                    continue;
                }
                text.append("method_").append(hits).append('=')
                    .append(sanitizeStateValue(m.toString())).append('\n');
                ++hits;
            }
            text.append("method_count=").append(hits).append('\n');
            text.append("invoked_getter_count=0\n");
            if (writeAtomicState(OEM_DISPLAYMANAGER_API_FILE, text.toString())) {
                displayManagerApiProbed = true;
                diag("OEM_DISPLAYMANAGER_API metadata captured methods=" + hits
                    + " invoked=0");
            }
        } catch (Throwable t) {
            diag("WARN OEM DisplayManager metadata probe failed: " + describe(t));
        }
    }

    private static Object resolveListModel176(Object hmi) throws Exception {
        /*
         * Stock K1004 evidence is NavigationEnv.getListModel(176).  The hook
         * is entered with IFrameworkAccess rather than a typed NavigationEnv,
         * so first test the read-only model seams already exposed by the HMI
         * runtime.  Never accept an object until it proves ListModel shape.
         */
        Object candidate = tryListModelOn(hmi, "HMIService");
        if (candidate != null) return candidate;

        candidate = tryListModelOn(frameworkAccess, "FrameworkAccess");
        if (candidate != null) return candidate;

        lastOemModelAccess = "UNRESOLVED_NAVIGATIONENV_SEAM";
        return null;
    }

    private static Object tryListModelOn(Object owner, String ownerName)
        throws Exception {
        if (owner == null) return null;
        Object candidate = null;

        /*
         * Treat each runtime seam independently.  A method can exist yet throw
         * from inside the proprietary HMI implementation; that must not prevent
         * the remaining read-only fallbacks from being attempted.
         */
        try {
            candidate = invokeInt(owner, "getListModel",
                                  OEM_SCREEN_LAYOUT_MODEL_ID);
        } catch (Throwable ignored) {
            candidate = null;
        }
        if (isListModelShape(candidate)) {
            lastOemModelAccess = ownerName + ".getListModel(176)";
            return candidate;
        }

        /*
         * A generic getModel(int) is also accepted, but only if its returned
         * object implements the expected getLength/getRow read interface.
         */
        try {
            candidate = invokeInt(owner, "getModel",
                                  OEM_SCREEN_LAYOUT_MODEL_ID);
        } catch (Throwable ignored) {
            candidate = null;
        }
        if (isListModelShape(candidate)) {
            lastOemModelAccess = ownerName + ".getModel(176)";
            return candidate;
        }
        return null;
    }

    private static boolean isListModelShape(Object candidate) {
        if (candidate == null) return false;
        try {
            Object length = invokeNoArg(candidate, "getLength");
            if (!(length instanceof Number)) return false;
            candidate.getClass().getMethod(
                "getRow", new Class[]{Integer.TYPE});
            return true;
        } catch (Throwable t) {
            return false;
        }
    }

    private static int[] readIntegerRow(Object row, int colCount)
        throws Exception {
        int limit = colCount;
        if (limit > 64) limit = 64;
        int[] values = new int[limit];
        int i;
        for (i = 0; i < limit; ++i) {
            values[i] = OEM_MISSING;
            try {
                Object cell = invokeInt(row, "getCell", i);
                if (cell == null) continue;
                Object value = invokeNoArg(cell, "getValue");
                if (value instanceof Number) {
                    values[i] = ((Number)value).intValue();
                }
            } catch (Throwable ignored) {
                values[i] = OEM_MISSING;
            }
        }
        return values;
    }

    private static boolean oemColumnsValid(int[] values) {
        int[] required = new int[]{
            0, 1, 8, 9, 11, 12, 13, 14, 24, 25, 26, 27
        };
        int i;
        for (i = 0; i < required.length; ++i) {
            int c = required[i];
            if (c >= values.length || values[c] == OEM_MISSING) return false;
        }
        return true;
    }

    private static boolean oemGeometrySane(int screenWidth,
                                                  int screenHeight,
                                                  int mapWidth,
                                                  int mapHeight,
                                                  int fullW,
                                                  int fullH,
                                                  int smallW,
                                                  int smallH) {
        /*
         * Fail closed on a false model seam or corrupt row.  Negative X/Y are
         * legitimate for some layouts, so only dimensions are constrained.
         */
        if (screenWidth <= 0 || screenHeight <= 0
            || screenWidth > 8192 || screenHeight > 8192)
            return false;
        if (mapWidth <= 0 || mapHeight <= 0
            || mapWidth > 8192 || mapHeight > 8192)
            return false;
        if (fullW <= 0 || fullH <= 0 || smallW <= 0 || smallH <= 0)
            return false;
        if (fullW > 16384 || fullH > 16384
            || smallW > 16384 || smallH > 16384)
            return false;
        return true;
    }

    private static String serializeIntegerRow(int[] values) {
        StringBuffer b = new StringBuffer();
        int i;
        for (i = 0; i < values.length; ++i) {
            if (i > 0) b.append(',');
            b.append(i).append(':');
            if (values[i] == OEM_MISSING) b.append('?');
            else b.append(values[i]);
        }
        return b.toString();
    }

    private static int readLayoutConstant(Object layout, int id)
        throws Exception {
        if (layout == null) throw new Exception("layout=null");
        Method getInt = cachedMethod(layout.getClass(), "getIntegerConstant", true);
        Object value = getInt.invoke(layout, new Object[]{new Integer(id)});
        if (!(value instanceof Number)) {
            throw new Exception("layout constant " + id + " non-number");
        }
        return ((Number)value).intValue();
    }

    private static int readLayoutConstantSafe(Object layout, int id) {
        try {
            return readLayoutConstant(layout, id);
        } catch (Throwable t) {
            return OEM_MISSING;
        }
    }

    private static int numberValue(Object value, int fallback) {
        return value instanceof Number ? ((Number)value).intValue() : fallback;
    }

    private static void oemProbeUnavailable(String reason) {
        String status = reason == null ? "unknown" : reason;
        if (!status.equals(lastOemProbeStatus)) {
            lastOemProbeStatus = status;
            diag("OEM_GEOMETRY_OBSERVER unavailable: " + status);
        }

        /*
         * Preserve a previously valid snapshot.  If no valid snapshot has
         * ever been published, expose an explicit invalid state so log
         * collection can distinguish 'not yet sampled' from a missing probe.
         */
        File dst = new File(OEM_GEOMETRY_STATE_FILE);
        if (!dst.exists()) {
            String text = "schema=1\n"
                + "observer=OEM_LAYOUT_OBSERVER_V1\n"
                + "mode=OBSERVE_ONLY\n"
                + "valid=0\n"
                + "timestamp_ms=" + nowMs() + "\n"
                + "reason=" + sanitizeStateValue(status) + "\n"
                + "apply_to_carplay=0\n"
                + "apply_to_renderer=0\n";
            writeAtomicState(OEM_GEOMETRY_STATE_FILE, text);
        }
    }

    private static String sanitizeStateValue(String value) {
        if (value == null) return "unknown";
        return value.replace('\n', ' ').replace('\r', ' ');
    }

    private static boolean writeAtomicState(String path, String text) {
        File tmp = new File(path + ".tmp");
        File dst = new File(path);
        FileOutputStream out = null;
        try {
            out = new FileOutputStream(tmp);
            out.write(text.getBytes("UTF-8"));
            out.flush();
            out.close();
            out = null;
            if (dst.exists() && !dst.delete()) {
                diag("WARN could not delete old state before replace path=" + path);
            }
            if (!tmp.renameTo(dst)) {
                copyFile(tmp, dst);
                tmp.delete();
            }
            return true;
        } catch (Throwable t) {
            diag("ERROR state write failed path=" + path + ": " + describe(t));
            try { if (out != null) out.close(); } catch (Throwable ignored) {}
            return false;
        }
    }

    private static int resolveNavViewSizeChoiceId() {
        if (navViewSizeChoiceResolved) return navViewSizeChoiceId;
        navViewSizeChoiceResolved = true;
        try {
            Class bank = Class.forName("de.audi.atip.model.ICoreNaviModelBank");
            Field f = bank.getField("NAV_VIEW_SIZE_CHOICE");
            navViewSizeChoiceId = f.getInt(null);
        } catch (Throwable t) {
            navViewSizeChoiceId = -1;
            diag("NAV_VIEW_SIZE_CHOICE reflection failed: " + describe(t));
        }
        return navViewSizeChoiceId;
    }

    private static Method cachedMethod(Class type, String name, boolean intParameter)
        throws NoSuchMethodException {
        synchronized (METHOD_LOCK) {
            int i;
            for (i = 0; i < methodCacheSize; ++i) {
                if (methodClasses[i] == type && methodIntParameters[i] == intParameter
                        && name.equals(methodNames[i])) return cachedMethods[i];
            }
            Method method = type.getMethod(name, intParameter ? INT_PARAMETER : NO_PARAMETERS);
            int slot = methodCacheNext;
            methodClasses[slot] = type;
            methodNames[slot] = name;
            methodIntParameters[slot] = intParameter;
            cachedMethods[slot] = method;
            methodCacheNext = (slot + 1) % METHOD_CACHE_LIMIT;
            if (methodCacheSize < METHOD_CACHE_LIMIT) ++methodCacheSize;
            return method;
        }
    }

    private static Object invokeNoArg(Object target, String name) throws Exception {
        if (target == null) return null;
        Method m = cachedMethod(target.getClass(), name, false);
        return m.invoke(target, NO_ARGUMENTS);
    }

    private static Object invokeInt(Object target, String name, int value)
        throws Exception {
        if (target == null) return null;
        Method m = cachedMethod(target.getClass(), name, true);
        return m.invoke(target, new Object[]{new Integer(value)});
    }

    private static void geometryReapply(String reason) {
        if (geometryClassMissing) return;
        try {
            Class c = Class.forName("com.luka.carplay.cluster.ClusterLayerController");
            Method m = c.getMethod("reapply", new Class[0]);
            m.invoke(null, new Object[0]);
        } catch (ClassNotFoundException e) {
            geometryClassMissing = true;
        } catch (NoSuchMethodException e) {
            geometryClassMissing = true;
        } catch (Throwable t) {
            diag("WARN geometry reapply failed reason=" + reason + ": " + describe(t));
        }
    }

    private static boolean writeHmiState(String layout, String view,
                                         String layoutName,
                                         int smallDx, int smallDy) {
        File tmp = new File(HMI_STATE_FILE + ".tmp");
        File dst = new File(HMI_STATE_FILE);
        FileOutputStream out = null;
        try {
            out = new FileOutputStream(tmp);
            String text = "layout=" + layout + "\n"
                + "view=" + view + "\n"
                + "layout_name=" + layoutName + "\n"
                + "small_stage_dx=" + smallDx + "\n"
                + "small_stage_dy=" + smallDy + "\n"
                + "carplay_session=" + (carPlaySessionActive ? "1" : "0") + "\n"
                + "cluster_owned=" + (isClusterOwned() ? "1" : "0") + "\n"
                + "rgi_active=" + (rgiPresentationActive ? "1" : "0") + "\n";
            out.write(text.getBytes("UTF-8"));
            out.flush();
            out.close();
            out = null;
            if (dst.exists() && !dst.delete()) {
                diag("WARN could not delete old state file before replace");
            }
            if (!tmp.renameTo(dst)) {
                copyFile(tmp, dst);
                tmp.delete();
            }
            return true;
        } catch (Throwable t) {
            diag("ERROR state write failed: " + describe(t));
            try { if (out != null) out.close(); } catch (Throwable ignored) {}
            return false;
        }
    }

    private static void observerStatus(String status) {
        if (status.equals(lastObserverStatus)) return;
        lastObserverStatus = status;
        diag("observer: " + status);
    }

    private static String readSmallState(String path, int maxBytes) {
        FileInputStream in = null;
        try {
            File f = new File(path);
            if (!f.exists() || maxBytes <= 0) return "";
            in = new FileInputStream(f);
            byte[] buf = new byte[maxBytes];
            int n = 0;
            while (n < buf.length) {
                int got = in.read(buf, n, buf.length - n);
                if (got <= 0) break;
                n += got;
            }
            in.close();
            in = null;
            if (n <= 0) return "";
            if (DISPLAYABLE3_STATE_FILE.equals(path))
                return DisplayStateSnapshot.decode(buf, n);
            return new String(buf, 0, n, "UTF-8");
        } catch (Throwable t) {
            try { if (in != null) in.close(); } catch (Throwable ignored) {}
            return "";
        }
    }

    private static String stateValue(String text, String key,
                                     String fallback) {
        if (text == null || key == null) return fallback;
        String prefix = key + "=";
        int from = 0;
        while (from < text.length()) {
            int end = text.indexOf('\n', from);
            if (end < 0) end = text.length();
            if (text.startsWith(prefix, from)) {
                String value = text.substring(from + prefix.length(), end);
                return sanitizeStateValue(value);
            }
            from = end + 1;
        }
        return fallback;
    }

    private static long stateLong(String text, String key, long fallback) {
        try {
            return Long.parseLong(stateValue(text, key,
                                            Long.toString(fallback)));
        } catch (Throwable ignored) {
            return fallback;
        }
    }

    private static String readContextMode() {
        BufferedReader reader = null;
        try {
            File f = new File(CONTEXT_MODE_FILE);
            if (!f.exists()) return MODE_JAVA80;
            reader = new BufferedReader(
                new InputStreamReader(new FileInputStream(f), "UTF-8"));
            String line = reader.readLine();
            reader.close();
            reader = null;
            if (line != null && MODE_JAVA80.equals(
                line.trim().toUpperCase())) return MODE_JAVA80;
        } catch (Throwable t) {
            try { if (reader != null) reader.close(); }
            catch (Throwable ignored) {}
            diag("context mode read failed; default JAVA80: " + describe(t));
        }
        return MODE_JAVA80;
    }

    private static void writeStartedMarker() {
        FileOutputStream out = null;
        try {
            out = new FileOutputStream(STARTED_FILE);
            String text = "started=1\n"
                + "time_ms=" + nowMs() + "\n"
                + "mode=JAVA80\n"
                + "ctx=80\n"
                + "ctx_readback=getCurrentContextID(1)\n";
            out.write(text.getBytes("UTF-8"));
            out.close();
        } catch (Throwable t) {
            try { if (out != null) out.close(); } catch (Throwable ignored) {}
        }
    }

    private static void diag(String text) {
        synchronized (DIAG_LOCK) {
            FileOutputStream out = null;
            try {
                File f = new File(DIAG_FILE);
                if (f.exists() && f.length() > DIAG_MAX_BYTES) {
                    FileOutputStream reset = new FileOutputStream(f, false);
                    reset.write(("--- log reset at " + nowMs() + " ---\n")
                        .getBytes("UTF-8"));
                    reset.close();
                }
                out = new FileOutputStream(f, true);
                String line = nowMs() + " " + text + "\n";
                out.write(line.getBytes("UTF-8"));
                out.close();
            } catch (Throwable ignored) {
                try { if (out != null) out.close(); } catch (Throwable ignored2) {}
            }
        }
    }

    private static String describe(Throwable t) {
        if (t == null) return "unknown";
        String m = t.getMessage();
        return t.getClass().getName() + (m == null ? "" : ": " + m);
    }

    private static void copyFile(File src, File dst) throws Exception {
        FileInputStream in = new FileInputStream(src);
        FileOutputStream out = new FileOutputStream(dst);
        byte[] buf = new byte[512];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        in.close();
        out.close();
    }

    private static long nowMs() {
        return System.currentTimeMillis();
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
