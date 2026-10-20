.pragma library

function unavailable(controller, key) {
    return (controller.info.unavailableModules || {})[key] || "";
}

function issue(controller, key) {
    if (unavailable(controller, key))
        return unavailable(controller, key);
    const issues = controller.info.moduleIssues || {};
    return issues[key] || "";
}

function connectionText(controller, key) {
    if (unavailable(controller, key))
        return "Unavailable on ARM64";
    if (issue(controller, key))
        return "Needs Attention";
    if (key === "haweb") {
        const web = controller.homeAssistantWeb;
        if (!web.enabled)
            return "Disabled";
        if (web.ready)
            return "Ready";
        if (web.loading)
            return "Loading…";
        return web.url ? "Ready to Open" : "Not configured";
    }
    if (key === "custom")
        return controller.customSources.enabled ? "Configured" : "Disabled";
    const status = controller.info;
    if (key === "wallpaper") {
        const state = controller.wallpaper;
        if (!state.enabled)
            return "Disabled";
        if (state.running)
            return "Connected";
        if (state.installed === false)
            return "Not configured";
        if (state.running === false)
            return "Not running";
        return "Checking…";
    }
    if (key === "beacn") {
        const beacn = controller.beacn;
        if (!beacn.enabled)
            return "Disabled";
        if (beacn.discovering)
            return "Discovering…";
        return beacn.ready ? "Connected" : "Not connected";
    }
    if (key === "windows") {
        if (!status.windowsEnabled)
            return "Disabled";
        return status.windowsHealthy ? "Collecting" : "Waiting for readings";
    }
    if (key === "coretemp") {
        if (!status.coretempEnabled)
            return "Disabled";
        return status.coretempConnected ? "Connected" : "Not connected";
    }
    if (key === "hwinfo") {
        if (!status.hwinfoEnabled)
            return "Disabled";
        return status.healthy ? "Connected" : "No current readings";
    }
    if (key === "nvidia") {
        if (!status.nvidiaEnabled)
            return "Disabled";
        return status.nvidiaConnected ? "Connected" : "No accessible GPU";
    }
    if (key === "icue") {
        if (!status.icueEnabled)
            return "Disabled";
        if (!status.icueDirectEnabled && !status.icueAutomationEnabled)
            return "Not configured";
        const direct = status.icueDirectEnabled && status.icueConnected;
        const automation = status.icueAutomationEnabled && status.icueAutomationConnected;
        if ((!status.icueDirectEnabled || direct) && (!status.icueAutomationEnabled || automation))
            return "Connected";
        return direct || automation ? "Partly connected" : "Not connected";
    }
    return "Not connected";
}

function enabled(controller, key) {
    if (unavailable(controller, key))
        return false;
    if (key === "haweb")
        return controller.homeAssistantWeb.enabled;
    if (key === "custom")
        return controller.customSources.enabled;
    if (key === "beacn")
        return controller.beacn.enabled === true;
    if (key === "wallpaper")
        return controller.wallpaper.enabled === true;
    return controller.info[key + "Enabled"] === true;
}

function statusColour(controller, key, theme) {
    if (!enabled(controller, key))
        return theme.muted;
    if (issue(controller, key))
        return theme.warning;
    // The embedded browser is created on demand. A configured, unopened view is healthy.
    if (key === "haweb" && controller.homeAssistantWeb.url)
        return theme.accent;
    return connected(controller, key) ? theme.accent : theme.muted;
}

function connected(controller, key) {
    if (key === "haweb")
        return controller.homeAssistantWeb.ready;
    const status = controller.info;
    switch (key) {
    case "custom":
        return controller.customSources.enabled;
    case "beacn":
        return controller.beacn.ready === true;
    case "wallpaper":
        return controller.wallpaper.running === true;
    case "hwinfo":
        return status.healthy;
    case "windows":
        return status.windowsHealthy;
    case "icue":
        return (status.icueDirectEnabled && status.icueConnected)
            || (status.icueAutomationEnabled && status.icueAutomationConnected);
    default:
        return status[key + "Connected"] === true;
    }
}
