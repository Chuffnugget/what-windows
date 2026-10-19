#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>

// Stable settings IDs and shader modes. Labels may change without changing a
// saved choice; the shader numbers match the original window motion studies.
namespace UiMotion {
struct Style {
    const char *id;
    const char *label;
    int mode;
};

inline constexpr Style styles[]{{"refraction", "Refraction Lens (Default)", 7},
                                {"rise", "Rise In", 0},
                                {"tidal", "Tidal Reveal", 1},
                                {"ink", "Ink Bloom", 2},
                                {"mercury", "Mercury Merge", 3},
                                {"mist", "Mist Condense", 4},
                                {"silk", "Silk Unfurl", 5},
                                {"contour", "Contour Wave", 6},
                                {"frost", "Frost Lattice", 8},
                                {"ember", "Ember Dissolve", 9},
                                {"prism", "Prism Assembly", 10},
                                {"pour", "Pour and Pool", 11},
                                {"molten", "Molten Glass Pull", 12},
                                {"precision", "Precision Fill", 13},
                                {"fresnel", "Fresnel Lens", 14},
                                {"anamorphic", "Anamorphic Lens", 15},
                                {"twin", "Twin Lens Fusion", 16},
                                {"meniscus", "Meniscus Lens", 17},
                                {"standard", "Standard", -1},
                                {"none", "Off", -2}};

inline const Style *find(const QString &id) {
    for (const auto &style : styles)
        if (id == QLatin1String(style.id))
            return &style;
    return nullptr;
}

inline const Style &resolve(const QString &id) {
    const auto *style = find(id);
    return style ? *style : styles[0];
}

inline const QVariantList &choices() {
    static const QVariantList result = [] {
        QVariantList list;
        for (const auto &style : styles)
            list.append(QVariantMap{{"id", style.id}, {"label", style.label}});
        return list;
    }();
    return result;
}
} // namespace UiMotion
