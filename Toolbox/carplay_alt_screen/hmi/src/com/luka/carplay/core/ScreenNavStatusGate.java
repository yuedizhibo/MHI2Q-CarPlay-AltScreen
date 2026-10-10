/*
 * ScreenNavStatusGate - installs the single wrapper that prevents stock
 * route-guidance BAP writes from racing CarPlay RGI.  It deliberately does not
 * alter NAVSD availability, initializing state, compass, scale or map status.
 */
package com.luka.carplay.core;

import com.luka.carplay.framework.Log;
import com.luka.carplay.routeguidance.GatedCombiService;

import de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi;
import de.audi.tghu.navi.app.Navigation;
import de.audi.tghu.navi.app.cluster.ClusterService;
import java.lang.reflect.Field;

public final class ScreenNavStatusGate {
    private static final String TAG = "StatusGate";

    private static ClusterService clusterService;
    private static GatedCombiService gate;
    private static boolean desiredRouteBlocked;
    private static boolean appliedRouteBlocked;
    private static boolean desiredCurrentPositionBlocked;
    private static boolean installScheduled;

    private ScreenNavStatusGate() { }

    /** All reads/writes of CombiBAPListener.combiservice must happen on NavigationJobs. */
    private static void scheduleInstallLocked() {
        if (installScheduled) return;
        Navigation navigation = Navigation.getInstance();
        if (navigation == null || navigation.getDispatcher() == null) return;
        installScheduled = true;
        try {
            navigation.getDispatcher().execute(new Runnable() {
                public void run() { installNow(); }
            });
        } catch (Throwable t) {
            installScheduled = false;
        }
    }

    private static synchronized void installNow() {
        installScheduled = false;
        try {
            Navigation navigation = Navigation.getInstance();
            if (navigation == null) return;
            ClusterService cs = navigation.getClusterService();
            if (cs == null) return;

            CombiBAPServiceNavi current = cs.getCombiBAPListenerCombiService();
            if (current == null) return;

            if (cs != clusterService || gate == null || current != gate) {
                if (current instanceof GatedCombiService) {
                    gate = (GatedCombiService) current;
                } else {
                    gate = new GatedCombiService(current);
                }
                gate.setRouteGuidanceBlocked(desiredRouteBlocked);
                gate.setCurrentPositionInfoBlocked(desiredCurrentPositionBlocked);
                if (current != gate) cs.setCombiBAPListenerCombiService(gate);
                clusterService = cs;
                seedInstrumentState(cs);
                Log.i(TAG, "native route-guidance gate installed/reused");
            }

            gate.setRouteGuidanceBlocked(desiredRouteBlocked);
            gate.setCurrentPositionInfoBlocked(desiredCurrentPositionBlocked);
            appliedRouteBlocked = desiredRouteBlocked;
        } catch (Throwable t) {
        }
    }

    /** Installing after stock updateAll must not wait for another Fct44/54 delta.
     * Read both accepted values on NavigationJobs, without sending synthetic BAP
     * responses. The fields can belong to the stock listener's superclass. */
    private static boolean readListenerBoolean(Object listener, String name) throws Exception {
        Class type = listener.getClass();
        while (type != null) {
            try {
                Field field = type.getDeclaredField(name);
                field.setAccessible(true);
                return field.getBoolean(listener);
            } catch (NoSuchFieldException absent) {
                type = type.getSuperclass();
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static void seedInstrumentState(ClusterService cs) {
        /* Withdraw a previous service's visibility until the new snapshot is known. */
        GatedCombiService.publishMapVisibility(false);
        try {
            Object listener = cs.getClass().getMethod("getCombiBAPListener", new Class[0])
                .invoke(cs, new Object[0]);
            boolean visible = readListenerBoolean(listener, "supplementaryMapViewVisible");
            boolean largeMapView = readListenerBoolean(listener, "largeMapView");
            GatedCombiService.publishMapPresentation(largeMapView);
            GatedCombiService.publishMapVisibility(visible);
            Log.i(TAG, "cluster display-state seeded visible=" + visible
                + " largeMapView=" + largeMapView);
        } catch (Throwable t) {
            try { Log.w(TAG, "cluster display-state seed unavailable; awaiting stock Status: " + t); }
            catch (Throwable ignored) { }
        }
    }

    /** Called synchronously by ClusterService#setCombiBAPService on NavigationJobs, before the
     * stock CombiBAPListener setter performs updateAll().  Installing here closes the old race in
     * which updateAll painted native INITIALIZING/status values before the asynchronous wrapper
     * appeared, and avoids cross-thread writes to CombiBAPListener.combiservice. */
    public static synchronized CombiBAPServiceNavi prepareCombiBAPService(
            ClusterService cs, CombiBAPServiceNavi incoming) {
        if (incoming == null) {
            if (cs == clusterService && gate != null) {
                try { gate.setRouteGuidanceBlocked(false); } catch (Throwable t) { }
                try { gate.setCurrentPositionInfoBlocked(false); } catch (Throwable t) { }
                clusterService = null;
                gate = null;
                appliedRouteBlocked = false;
                GatedCombiService.publishMapVisibility(false);
            }
            return null;
        }

        GatedCombiService next;
        if (incoming instanceof GatedCombiService) {
            next = (GatedCombiService) incoming;
        } else {
            next = new GatedCombiService(incoming);
        }
        if (gate != null && gate != next) {
            try { gate.setRouteGuidanceBlocked(false); } catch (Throwable t) { }
        }
        clusterService = cs;
        gate = next;
        gate.setRouteGuidanceBlocked(desiredRouteBlocked);
        gate.setCurrentPositionInfoBlocked(desiredCurrentPositionBlocked);
        seedInstrumentState(cs);
        appliedRouteBlocked = desiredRouteBlocked;
        installScheduled = false;
        Log.i(TAG, "native route-guidance gate installed on Navigation service edge");
        return gate;
    }

    /** Single owner of the native CombiBAP wrapper for both the cluster module and RGI. */
    public static synchronized boolean setRouteGuidanceBlocked(boolean blocked) {
        boolean changed = desiredRouteBlocked != blocked;
        desiredRouteBlocked = blocked;
        if (!blocked) {
            desiredCurrentPositionBlocked = false;
            if (gate != null) gate.setCurrentPositionInfoBlocked(false);
        }
        if (gate == null || changed || appliedRouteBlocked != blocked) scheduleInstallLocked();
        return gate != null && appliedRouteBlocked == blocked && !installScheduled;
    }

    /** FctID 19/20/21/22/46 ownership follows actual CarPlay RGI, not the
     * session-long native-RG gate. The wrapper is already installed on
     * NavigationJobs, so changing these volatile policy bits requires no
     * service-field mutation. */
    public static synchronized void setCurrentPositionInfoBlocked(boolean blocked) {
        desiredCurrentPositionBlocked = blocked;
        if (gate != null) gate.setCurrentPositionInfoBlocked(blocked);
    }

    /** Exact stock-service availability edge, after prepareCombiBAPService + stock updateAll.
     * Also wakes CarPlayApp so RgdModule releases/reacquires its paired OSGi reference when the
     * Navigation service is replaced without a phone reconnect. */
    public static synchronized void onCombiBAPServiceChanged(ClusterService cs) {
        CombiBAPServiceNavi current = null;
        try { if (cs != null) current = cs.getCombiBAPListenerCombiService(); }
        catch (Throwable t) { }

        if (cs != clusterService || current != gate) {
            appliedRouteBlocked = false;
        }
        if (current == null) {
            if (cs == clusterService) {
                clusterService = null;
                gate = null;
                GatedCombiService.publishMapVisibility(false);
            }
            /* Resident CarPlayHook retries unavailable navigation services. */
            return;
        }
        if (desiredRouteBlocked) scheduleInstallLocked();
        /* Resident CarPlayHook retries unavailable navigation services. */
    }
}
