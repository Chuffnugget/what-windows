.pragma library

// Only direct siblings participate. Opening a nested section keeps its parent
// open and does not change module enablement or any saved settings.
function closeSiblings(section) {
    if (!section.parent)
        return;
    const siblings = section.parent.children;
    for (let i = 0; i < siblings.length; ++i) {
        const sibling = siblings[i];
        if (sibling !== section && typeof sibling.collapseSection === "function")
            sibling.collapseSection();
    }
}
