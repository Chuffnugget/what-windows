#version 440

// Open/close shapes adapted from the window motion demo. Each branch runs only
// for the selected effect; the QML layer is released when the transition ends.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float progress;
    int mode;
    vec2 resolution;
    float strength;
    vec4 sourceRect;
    vec4 tint;
    float blurRadius;
    float sourcePixelRatio;
    bool preserveBackground;
    vec4 backgroundRect;
};

layout(binding = 1) uniform sampler2D source;
layout(binding = 2) uniform sampler2D sharpBackground;

const float PI = 3.14159265359;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), f.x), f.y);
}

float fbm(vec2 p) {
    return noise2(p) * 0.57 + noise2(p * 2.07 + 8.3) * 0.28 + noise2(p * 4.19 - 2.8) * 0.15;
}

float smin(float a, float b, float k) {
    float h = max(k - abs(a - b), 0.0) / k;
    return min(a, b) - h * h * k * 0.25;
}

float box(vec2 p, vec2 b) {
    vec2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

float fill(float d, float aa) {
    return 1.0 - smoothstep(-aa, aa, d);
}

float rim(float d, float width) {
    return exp(-abs(d) / max(width, 0.0001));
}

vec4 backgroundAt(vec2 uv) {
    vec2 point = (sourceRect.xy + uv * sourceRect.zw - backgroundRect.xy) / max(backgroundRect.zw, vec2(1.0));
    float inside = step(0.0, point.x) * step(point.x, 1.0) * step(0.0, point.y) * step(point.y, 1.0);
    return texture(sharpBackground, clamp(point, vec2(0.0), vec2(1.0))) * inside;
}

// Pair adjacent Gaussian samples through bilinear filtering: a dense 9x9
// footprint needs 25 texture reads in the existing pass. Work in texture pixels
// so display scaling cannot spread samples apart and leave ghost text edges.
vec4 sampleContent(vec2 uv) {
    uv = clamp(uv, vec2(0.0), vec2(1.0));
    if (blurRadius <= 0.01)
        return texture(source, uv);
    vec2 texturePixels = ceil(sourceRect.zw * sourcePixelRatio);
    float sigma = max(0.01, blurRadius * 0.4 * texturePixels.y / sourceRect.w);
    float falloff = -0.5 / (sigma * sigma);
    vec4 gaussian = exp(vec4(1.0, 4.0, 9.0, 16.0) * falloff);
    float nearWeight = gaussian.x + gaussian.y;
    float farWeight = gaussian.z + gaussian.w;
    float nearOffset = 1.0 + gaussian.y / max(nearWeight, 0.000001);
    float farOffset = 3.0 + gaussian.w / max(farWeight, 0.000001);
    float positions[5] = float[](-farOffset, -nearOffset, 0.0, nearOffset, farOffset);
    float weights[5] = float[](farWeight, nearWeight, 1.0, nearWeight, farWeight);
    float total = 1.0 + 2.0 * (nearWeight + farWeight);
    vec4 colour = vec4(0.0);
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 5; ++x) {
            float weight = weights[x] * weights[y] / (total * total);
            vec2 point = clamp(uv + vec2(positions[x], positions[y]) / texturePixels, vec2(0.0), vec2(1.0));
            vec4 sampleColour = texture(source, point);
            if (preserveBackground)
                sampleColour -= backgroundAt(point);
            colour += sampleColour * weight;
        }
    }
    // Subtract the background before filtering, then restore its sharp sample.
    // This preserves borders and glass gradients without splitting the popup.
    if (preserveBackground)
        colour += backgroundAt(uv);
    colour.a = clamp(colour.a, 0.0, 1.0);
    colour.rgb = clamp(colour.rgb, vec3(0.0), vec3(colour.a));
    return colour;
}

vec2 edgeNormal(float field) {
    vec2 gradient = vec2(dFdx(field), dFdy(field));
    return gradient / max(length(gradient), 0.00001);
}

void main() {
    vec2 uv = (sourceRect.xy + qt_TexCoord0 * sourceRect.zw) / resolution;
    float p = clamp(progress, 0.0, 1.0);
    vec4 original = texture(source, qt_TexCoord0);
    // Exact endpoints prevent a flash, residual glow, or distortion at either handoff.
    if (p <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    if (p >= 1.0) {
        fragColor = original * qt_Opacity;
        return;
    }

    // Collapsible sections and Standard popups keep their geometry animation.
    if (mode < 0) {
        fragColor = sampleContent(qt_TexCoord0) * qt_Opacity;
        return;
    }

    float aspect = resolution.x / max(resolution.y, 1.0);
    vec2 metric = vec2(aspect, 1.0);
    vec2 q = (uv - 0.5) * metric;
    float diagonal = length(metric * 0.5);
    float aa = 1.3 / max(resolution.y, 1.0);
    float settle = 1.0 - smoothstep(0.72, 1.0, p);
    float reveal = 1.0, light = 0.0, shade = 1.0;
    vec2 sampleUv = uv;
    vec2 dispersion = vec2(0.0);
    float glassField = 100.0;
    float glassStrength = 1.0;

    if (mode == 0) {
        // Move within the transparent surface, without changing saved window geometry.
        sampleUv.y -= 0.10 * pow(1.0 - p, 3.0);
        reveal = smoothstep(0.03, 0.42, p);
    } else if (mode == 1) {
        float waves = sin(uv.x * 13.0 + p * 9.0) * 0.031 + sin(uv.x * 29.0 - p * 12.0) * 0.013;
        float fingers = (fbm(vec2(uv.x * 9.0, p * 3.0)) - 0.5) * 0.14;
        float front = 1.15 - 1.36 * p + (waves + fingers) * sin(PI * p);
        float d = front - uv.y;
        reveal = fill(d, 0.009);
        light = rim(d, 0.007) * 0.58 + rim(d + 0.027, 0.025) * 0.13;
        sampleUv.x += sin(uv.y * 45.0 + p * 15.0) * 0.008 * rim(d, 0.11) * settle;
    } else if (mode == 2) {
        vec2 ink = (uv - vec2(0.46, 0.59)) * metric;
        float angle = atan(ink.y, ink.x);
        float rough = (fbm(uv * 16.0) - 0.5) * 0.17 + sin(angle * 9.0) * 0.025;
        float d = length(ink) + rough * sin(PI * p) - p * (diagonal + 0.36);
        reveal = fill(d, 0.019);
        light = rim(d, 0.018) * (0.18 + 0.18 * noise2(uv * 75.0));
        shade = 0.70 + 0.30 * smoothstep(-0.02, 0.12, -d);
    } else if (mode == 3) {
        float radius = p * diagonal * 0.94;
        vec2 c1 = mix(vec2(0.16, 0.84), vec2(0.31, 0.46), p);
        vec2 c2 = mix(vec2(0.83, 0.72), vec2(0.69, 0.50), p);
        vec2 c3 = mix(vec2(0.35, 0.10), vec2(0.42, 0.35), p);
        vec2 c4 = mix(vec2(0.76, 0.18), vec2(0.65, 0.34), p);
        float d = length((uv - c1) * metric) - radius;
        d = smin(d, length((uv - c2) * metric) - radius * 0.91, 0.13);
        d = smin(d, length((uv - c3) * metric) - radius * 0.77, 0.13);
        d = smin(d, length((uv - c4) * metric) - radius * 0.64, 0.11);
        reveal = fill(d, aa);
        light = rim(d, 0.009) * 0.76 + rim(d + 0.024, 0.012) * 0.19;
        shade = 1.0 - rim(d + 0.045, 0.022) * 0.40;
        sampleUv +=
            vec2(sin(uv.y * 22.0 + p * 6.0), cos(uv.x * 18.0 - p * 7.0)) * 0.012 * rim(d, 0.12) * settle;
    } else if (mode == 4) {
        vec2 drift = uv * vec2(5.0 * aspect, 5.0) + vec2(p * 0.70, -p * 0.55);
        float cloud = fbm(drift + vec2(noise2(drift + 4.0), noise2(drift - 3.0)) * 1.3);
        float d = cloud - mix(-0.17, 1.18, p);
        reveal = fill(d, 0.14);
        sampleUv += vec2(noise2(drift + 11.0) - 0.5, noise2(drift - 7.0) - 0.5) * 0.07 * (1.0 - p) * settle;
        light = rim(d, 0.10) * 0.11;
    } else if (mode == 5) {
        float fold = sin(uv.y * 11.0 + p * 7.0) * 0.050 + sin(uv.y * 23.0 - p * 4.0) * 0.012;
        float front = -0.12 + 1.27 * p + fold * sin(PI * p);
        float d = uv.x - front;
        reveal = fill(d, aa);
        float curl = rim(d, 0.060) * settle;
        sampleUv.x -= curl * 0.09;
        sampleUv.y += sin(uv.y * 11.0 + p * 7.0) * 0.018 * curl;
        shade = 1.0 - rim(d + 0.036, 0.025) * 0.47 * settle;
        light = rim(d + 0.007, 0.009) * 0.48;
    } else if (mode == 6) {
        vec2 origin = (uv - vec2(0.45, 0.78)) * metric;
        float field = length(origin) + (fbm(uv * 5.0) - 0.5) * 0.16;
        float d = field - p * (diagonal + 0.43);
        reveal = fill(d, 0.013);
        float contours = pow(0.5 + 0.5 * cos(d * 160.0), 12.0);
        light = contours * exp(-abs(d) / 0.10) * 0.55;
    } else if (mode == 7) {
        vec2 lens = (uv - vec2(0.48, 0.57)) * metric;
        float radius = p * (diagonal + 0.22);
        float radial = length(lens);
        float d = radial - radius;
        reveal = fill(d, 0.006);
        vec2 normal = lens / max(radial, 0.001);
        sampleUv -= normal / metric * 0.060 * rim(d + 0.035, 0.090) * settle;
        light = rim(d, 0.007) * 0.64 + rim(d + 0.026, 0.008) * 0.19;
        shade = 1.0 - rim(d + 0.055, 0.030) * 0.18 * settle;
    } else if (mode == 8) {
        vec2 grid = uv * vec2(aspect, 1.0) * 12.0;
        float crystal = 100.0, branches = 0.0;
        vec2 base = floor(grid);
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                vec2 id = base + vec2(x, y);
                vec2 center = id + vec2(0.5) + vec2(hash21(id), hash21(id + 9.0)) * 0.24;
                vec2 local = grid - center;
                float seed = hash21(id + 31.0);
                float growth = smoothstep(seed * 0.4, 0.65 + seed * 0.3, p);
                float a = atan(local.y, local.x) + seed * 1.4;
                float r = length(local);
                float hex = r * cos(mod(a + PI / 6.0, PI / 3.0) - PI / 6.0);
                crystal = min(crystal, hex - growth * 0.90);
                float vein = abs(sin(a * 3.0)) * r;
                branches = max(branches, fill(vein - 0.019, 0.015) * fill(r - growth * 1.2, 0.02));
            }
        reveal = max(fill(crystal, 0.025), branches * 0.75);
        light = rim(crystal, 0.040) * 0.44 + branches * 0.24 * settle;
    } else if (mode == 9) {
        float field = fbm(uv * vec2(8.0 * aspect, 8.0));
        field += noise2(uv * vec2(61.0, 47.0)) * 0.11;
        float d = field - mix(-0.12, 1.25, p);
        reveal = fill(d, 0.025);
        light = rim(d, 0.025) * 0.83;
        float coals = pow(noise2(uv * vec2(130.0, 95.0) + p * 3.0), 9.0) * rim(d - 0.04, 0.07);
        light += coals * 0.55;
        shade = 1.0 - rim(d + 0.035, 0.025) * 0.65 * settle;
    } else if (mode == 10) {
        vec2 grid = vec2(9.0, 7.0), cell = floor(uv * grid), f = fract(uv * grid);
        float side = step(f.x, f.y);
        vec2 seedCell = cell + vec2(side * 23.0, side * 41.0);
        float seed = hash21(seedCell);
        float arrival = smoothstep(seed * 0.38, 0.57 + seed * 0.37, p);
        vec2 center = side > 0.5 ? vec2(0.333, 0.667) : vec2(0.667, 0.333);
        vec2 scaled = (f - center) / max(arrival, 0.001) + center;
        float triangle = max(max(-scaled.x, -scaled.y), max(scaled.x - 1.0, scaled.y - 1.0));
        triangle = max(triangle, side > 0.5 ? scaled.x - scaled.y : scaled.y - scaled.x);
        reveal = fill(triangle, 0.012);
        vec2 displacement = vec2(sin(seed * 39.0), cos(seed * 29.0)) * (1.0 - arrival) * 0.10;
        sampleUv += displacement * settle;
        light = rim(triangle, 0.015) * 0.4 * (1.0 - arrival);
        shade = mix(0.62 + seed * 0.25, 1.0, arrival);
    } else if (mode == 11) {
        // A travelling stream feeds a finite pool; its volume grows only after impact.
        float collected = smoothstep(0.17, 0.98, p);
        float slosh =
            (sin(uv.x * 15.0 + p * 10.0) + 0.35 * sin(uv.x * 33.0 - p * 13.0)) * 0.013 * sin(PI * p);
        float surface = 1.04 - collected * 1.20 + slosh;
        float poolWidth = aspect * (0.025 + 0.61 * smoothstep(0.10, 0.69, p));
        float pool = -smin(-(surface - uv.y), -(abs(uv.x - 0.5) * aspect - poolWidth), 0.055);
        float streamCenter = 0.5 + 0.035 * sin(uv.y * 8.0 - p * 8.0) * sin(PI * p);
        float streamWidth = (0.014 + 0.005 * sin(uv.y * 27.0 - p * 19.0)) * (1.0 - smoothstep(0.59, 0.92, p));
        float head = -0.12 + 1.25 * smoothstep(0.0, 0.22, p);
        float stream = max(abs(uv.x - streamCenter) * aspect - streamWidth, uv.y - head);
        stream += 0.12 * smoothstep(0.80, 0.94, p);
        float d = smin(pool, stream, 0.045);
        glassField = d;
        reveal = fill(d, aa);
        float impact = exp(-length((uv - vec2(streamCenter, max(surface, 0.03))) * metric) / 0.045);
        light = impact * 0.52 * sin(PI * p);
        sampleUv.x += sin(uv.y * 46.0 - p * 23.0) * 0.009 * settle * rim(stream, 0.065);
        sampleUv.y += sin(uv.x * 24.0 - p * 10.0) * 0.012 * settle * rim(pool, 0.12);
    } else if (mode == 12) {
        // A diagonal glass filament widens at its anchors and stretches at its waist.
        vec2 axis = normalize(vec2(0.80 * aspect, -0.76));
        vec2 normal = vec2(-axis.y, axis.x);
        float along = dot(q, axis), across = dot(q, normal);
        float halfLength = 0.008 + smoothstep(0.0, 0.45, p) * (diagonal + 0.13);
        float pulled = smoothstep(0.23, 0.98, p);
        float width = 0.003 + pulled * diagonal * 1.40;
        float shoulder = 0.48 + 0.52 * pow(clamp(abs(along) / halfLength, 0.0, 1.0), 1.6);
        float ripple = sin(along * 11.0 + p * 10.0) * 0.017 * sin(PI * p);
        float side = abs(across + ripple) - width * shoulder;
        float d = -smin(-side, -(abs(along) - halfLength), 0.020);
        glassField = d;
        reveal = fill(d, aa);
        sampleUv += normal / metric * (across * 0.16 + sin(along * 9.0 + p * 8.0) * 0.012) * settle;
        light = rim(side + 0.032, 0.010) * 0.27;
        glassStrength = 1.35;
    } else if (mode == 13) {
        // A laminar feed reaches the base, spreads into a reservoir, then raises
        // a planar meniscus. Only the light packets move along the straight jet.
        float spread = smoothstep(0.11, 0.37, p);
        float collected = smoothstep(0.27, 0.93, p);
        float surface = 0.987 - collected * 1.04;
        float poolWidth = aspect * (0.014 + 0.51 * spread);
        float bottom = 1.06;
        float centerY = (surface + bottom) * 0.5;
        float halfHeight = (bottom - surface) * 0.5;
        float corner = 0.006;
        float pool =
            box(vec2(q.x, uv.y - centerY), max(vec2(poolWidth, halfHeight) - corner, vec2(0.001))) - corner;
        // Hold the reservoir below the surface until the feed has reached it.
        pool += (1.0 - smoothstep(0.10, 0.17, p)) * 0.10;

        float feed = 1.0 - smoothstep(0.70, 0.91, p);
        float head = -0.06 + 1.10 * smoothstep(0.015, 0.17, p);
        float stream =
            box(vec2(q.x, uv.y - (head - 0.06) * 0.5), vec2(0.007 * feed, max((head + 0.06) * 0.5, 0.0001)));
        stream += smoothstep(0.84, 0.94, p) * 0.04;
        float d = smin(pool, stream, 0.005);
        glassField = d;
        glassStrength = 0.28;
        reveal = fill(d, aa);

        float across = fill(abs(q.x) - poolWidth, aa);
        float levelEdge = uv.y - surface;
        float levelActive = smoothstep(0.17, 0.29, p);
        float scanLine = rim(levelEdge, 0.0018) * 0.52;
        float trailingLine = rim(levelEdge - 0.012, 0.0015) * 0.13;
        float tickPosition = fract((uv.x - 0.5) * resolution.x / 22.0 + 0.5);
        float ticks = 1.0 - smoothstep(0.025, 0.10, abs(tickPosition - 0.5));
        float tickHeight = fill(abs(levelEdge - 0.006) - 0.006, aa);
        light = (scanLine + trailingLine + ticks * tickHeight * 0.13) * across * levelActive;

        float packetPosition = fract(uv.y * 4.5 - p * 9.0);
        float packets = 1.0 - smoothstep(0.04, 0.13, abs(packetPosition - 0.5));
        float exposedFeed = fill(uv.y - head, aa) * fill(uv.y - surface, aa) * feed;
        light += rim(q.x, 0.0020) * (0.16 + packets * 0.30) * exposedFeed;
        light += rim(q.x, 0.016) * rim(levelEdge, 0.006) * feed * levelActive * 0.24;
        // A small, steady bevel keeps the glass depth without a ripple deformation.
        sampleUv.y -= rim(levelEdge - 0.006, 0.016) * 0.0025 * settle * across;
        shade = 1.0 - rim(levelEdge - 0.024, 0.012) * 0.09 * settle * across;
    } else if (mode == 14) {
        // Six expanding Fresnel zones have independent curved optical profiles.
        vec2 lens = (uv - vec2(0.5, 0.53)) * metric;
        float radial = length(lens);
        vec2 normal = lens / max(radial, 0.001);
        float radius = (diagonal + 0.12) * smoothstep(0.0, 0.94, p);
        float d = radial - radius;
        float face = fill(d + aa * 2.0, aa);
        float zone = radial / max(radius, 0.001) * 6.0;
        float facet = fract(zone);
        float profile = 4.0 * facet * (1.0 - facet);
        float grooveDistance = min(facet, 1.0 - facet) * radius / 6.0;
        float grooves = rim(grooveDistance, max(aa, 0.0018));
        float machining = smoothstep(0.06, 0.22, p) * settle * face;
        float glint = 0.25 + 0.75 * pow(max(dot(normal, normalize(vec2(-0.7, -0.9))), 0.0), 4.0);

        glassField = d;
        glassStrength = 0.85;
        reveal = fill(d, aa);
        sampleUv -= normal / metric * (0.018 + 0.028 * profile) * machining;
        dispersion += normal / metric * 0.0022 * profile * machining;
        light = grooves * (0.10 + 0.22 * glint) * machining;
        shade = 1.0 - (0.07 * grooves + 0.05 * (1.0 - profile)) * machining;
    } else if (mode == 15) {
        // A cylindrical lens first extends sideways, then releases its vertical squeeze.
        vec2 lens = (uv - vec2(0.5, 0.52)) * metric;
        vec2 axes =
            vec2(0.004 + aspect * 0.82 * smoothstep(0.0, 0.52, p), 0.003 + 0.82 * smoothstep(0.14, 0.94, p));
        float d = (length(lens / axes) - 1.0) * min(axes.x, axes.y);
        float edgeAA = max(aa, fwidth(d) * 0.7);
        float face = fill(d, edgeAA);
        float cylinder = clamp(lens.y / axes.y, -1.0, 1.0);
        float thickness = max(0.0, 1.0 - cylinder * cylinder);
        float curvedEdge = rim(d + 0.025, 0.065) * settle;

        glassField = d;
        glassStrength = 0.65;
        reveal = face;
        sampleUv.y -= (lens.y * 0.42 * thickness * settle + sign(lens.y) * 0.038 * curvedEdge) * face;
        sampleUv.x -= lens.x / metric.x * 0.09 * thickness * settle * face;
        dispersion += vec2(0.0, cylinder) * 0.0035 * curvedEdge;
        float slit = rim(lens.y, max(aa, 0.0025)) * fill(abs(lens.x) - axes.x, aa);
        light = slit * 0.22 * (1.0 - smoothstep(0.15, 0.45, p));
        light += rim(d + 0.016, 0.004) * 0.18 * settle;
        shade = 1.0 - rim(d + 0.055, 0.025) * 0.20 * settle;
    } else if (mode == 16) {
        // Independent focal centres converge; their shared seam disappears before alignment.
        float separation = 1.0 - smoothstep(0.12, 0.73, p);
        vec2 offset = vec2(aspect * 0.25, 0.075) * separation;
        vec2 leftLens = q + offset;
        vec2 rightLens = q - offset;
        float leftRadius = length(leftLens);
        float rightRadius = length(rightLens);
        float radius = (diagonal + 0.12) * smoothstep(0.01, 0.94, p);
        float leftField = leftRadius - radius;
        float rightField = rightRadius - radius;
        float d = smin(leftField, rightField, 0.045);
        float leftWeight = smoothstep(-0.04, 0.04, rightField - leftField);
        vec2 focal = mix(rightLens, leftLens, leftWeight);
        vec2 normal = mix(rightLens / max(rightRadius, 0.001), leftLens / max(leftRadius, 0.001), leftWeight);
        float face = fill(d, aa);
        float thickness = max(0.0, 1.0 - pow(clamp(length(focal) / max(radius, 0.001), 0.0, 1.0), 2.0));
        float jointLife = smoothstep(0.25, 0.40, p) * (1.0 - smoothstep(0.45, 0.69, p));
        float overlap = fill(max(leftField, rightField), aa);
        float joint = rim(leftField - rightField, 0.018) * overlap * jointLife;
        float crossingRims =
            (rim(leftField, 0.004) * fill(rightField, aa) + rim(rightField, 0.004) * fill(leftField, aa)) *
            jointLife;

        glassField = d;
        glassStrength = 1.10;
        reveal = face;
        sampleUv -= focal / metric * 0.24 * thickness * settle * face;
        sampleUv += normal / metric * 0.018 * joint;
        dispersion += normal / metric * 0.0030 * (rim(d + 0.02, 0.08) + joint) * settle;
        light = joint * 0.32 + crossingRims * 0.24;
        shade = 1.0 - joint * 0.12;
    } else if (mode == 17) {
        // An annular lens grows outward while its central opening seals inward.
        vec2 lens = (uv - vec2(0.5, 0.53)) * metric;
        float radial = length(lens);
        vec2 normal = lens / max(radial, 0.001);
        float outerRadius = (diagonal + 0.12) * smoothstep(0.0, 0.78, p);
        float seal = smoothstep(0.26, 0.87, p);
        // A negative hole radius fully covers the centre before the final handoff.
        float holeRadius = outerRadius * 0.68 * (1.0 - seal) - 0.02 * seal;
        float outerField = radial - outerRadius;
        float innerField = holeRadius - radial;
        float d = max(outerField, innerField);
        float outerBevel = rim(outerField + 0.025, 0.07);
        float innerBevel = rim(innerField + 0.025, 0.06) * (1.0 - smoothstep(0.78, 0.88, p));
        float face = fill(d, aa);
        float glint = pow(max(dot(normal, normalize(vec2(-0.65, -0.76))), 0.0), 8.0);

        glassField = d;
        glassStrength = 0.90;
        reveal = face;
        sampleUv -= normal / metric * (0.047 * outerBevel - 0.070 * innerBevel) * settle * face;
        dispersion += normal / metric * 0.004 * (outerBevel - innerBevel) * settle;
        light = (rim(outerField, 0.003) * 0.40 + rim(innerField, 0.003) * 0.35) * glint;
        shade = 1.0 - (outerBevel + innerBevel) * 0.10 * settle;
    } else if (mode == 18) {
        // The QML slide already clips the reveal. Bend content along its edge
        // without a second mask. Pixel-sized offsets keep tall panels restrained.
        float distance = (uv.y - p) * resolution.y;
        float bevel = (1.0 - smoothstep(0.0, 40.0, abs(distance + 12.0))) * settle;
        vec2 inset = min(uv, vec2(1.0) - uv) * resolution;
        bevel *= smoothstep(8.0, 16.0, min(inset.x, inset.y));
        sampleUv += vec2(sin(uv.x * PI * 2.0) * 4.0, 4.0) * bevel / resolution;
    }

    if (mode >= 11 && mode < 18) {
        // A curved bevel bends the image and separates wavelengths at the material edge.
        vec2 normal = edgeNormal(glassField);
        float bevel = rim(glassField + 0.014, 0.037) * settle * glassStrength;
        sampleUv -= normal / metric * 0.025 * bevel;
        dispersion += normal / metric * 0.0035 * bevel;
        float reflection = 0.55 + 0.45 * pow(max(dot(normal, normalize(vec2(-0.6, -0.8))), 0.0), 3.0);
        light += rim(glassField, 0.004) * 0.62 * reflection;
        light += rim(glassField + 0.011, 0.003) * 0.19;
        shade *= 1.0 - rim(glassField + 0.036, 0.018) * 0.22 * settle;
    }

    // Seal every mode smoothly, including arbitrarily wide or tall windows.
    float finish = smoothstep(0.86, 1.0, p);
    // Keep the bend visible during short transitions without extending their duration.
    const float distortionGain = 2.0;
    sampleUv = mix(uv, sampleUv, distortionGain * strength * (1.0 - finish));
    sampleUv = (sampleUv * resolution - sourceRect.xy) / sourceRect.zw;
    reveal = mix(clamp(reveal, 0.0, 1.0), 1.0, finish);
    shade = mix(1.0, shade, strength * (1.0 - finish));
    float inside =
        step(0.0, sampleUv.x) * step(sampleUv.x, 1.0) * step(0.0, sampleUv.y) * step(sampleUv.y, 1.0);
    vec4 texel = sampleContent(sampleUv);
    if (mode == 18 && preserveBackground) {
        // Slide refraction bends the contents; the surrounding frame stays put.
        texel = clamp(texel - backgroundAt(sampleUv) + backgroundAt(qt_TexCoord0), 0.0, 1.0);
        texel.rgb = min(texel.rgb, vec3(texel.a));
    }
    if (mode >= 11 && mode < 18) {
        dispersion *= distortionGain * strength * (1.0 - finish) * resolution / sourceRect.zw;
        vec4 red = sampleContent(sampleUv + dispersion);
        vec4 blue = sampleContent(sampleUv - dispersion);
        texel.rgb = min(vec3(red.r, texel.g, blue.b), vec3(texel.a));
    }
    vec4 body = texel * (reveal * inside);
    body.rgb *= clamp(shade, 0.0, 1.0);
    float glow = clamp(light * strength, 0.0, 0.85) * (1.0 - finish);
    // Premultiplied 'over': RGB and alpha agree even along translucent liquid rims.
    vec4 composed = vec4(tint.rgb * glow, glow) + body * (1.0 - glow);
    fragColor = composed * smoothstep(0.0, 0.065, p) * qt_Opacity;
}
