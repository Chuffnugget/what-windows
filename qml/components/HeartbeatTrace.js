.pragma library

// Split a line, quadratic or cubic at the next reply's x coordinate. Only the
// undrawn tail is replaced; points behind the drawing tip keep their geometry.
function splitAtX(points, boundary) {
    let low = 0;
    let high = 1;
    let left;
    for (let attempt = 0; attempt < 16; ++attempt) {
        const t = (low + high) / 2;
        let level = points;
        left = [level[0]];
        while (level.length > 1) {
            const next = [];
            for (let i = 0; i + 1 < level.length; ++i) {
                next.push([
                    level[i][0] + (level[i + 1][0] - level[i][0]) * t,
                    level[i][1] + (level[i + 1][1] - level[i][1]) * t
                ]);
            }
            level = next;
            left.push(level[0]);
        }
        if (left[left.length - 1][0] < boundary)
            low = t;
        else
            high = t;
    }
    left[left.length - 1][0] = boundary;
    return left;
}

function path(beats, started, millisecondsPerPixel, width, baseline) {
    const geometry = [started, millisecondsPerPixel, width, baseline].join("/");
    let previous = [-width, baseline];
    let result = "M " + previous.join(" ");
    for (let i = 0; started > 0 && i < beats.length; ++i) {
        const beat = beats[i];
        // The spacing was captured when this reply arrived. A later reply must
        // never shrink an earlier peak or move it towards the start of its beat.
        const scale = Math.min(beat.width, beat.spacingMs / millisecondsPerPixel / 48);
        const start = (beat.time - started) / millisecondsPerPixel;
        const x = start + 18 * scale;
        if (x + 24 * scale < 0 || start > width)
            continue;
        const limit = i + 1 < beats.length
            ? (beats[i + 1].time - started) / millisecondsPerPixel : Infinity;
        // An overlapping beat continues from the cut tail. Otherwise connect
        // the two complete strokes along the baseline as usual.
        if (previous[0] !== start) {
            previous = [start, baseline];
            result += " L " + previous.join(" ");
        }
        const startY = previous[1];
        // A reply changes only the last tail and the new beat. Reuse older SVG
        // commands so long histories do not repeat curve work on the GUI thread.
        const cached = beat.pathCache;
        if (cached && cached.geometry === geometry && cached.limit === limit && cached.startY === startY) {
            result += cached.commands;
            previous = cached.end;
            continue;
        }
        const point = (dx, dy) => [x + dx * scale, baseline + dy];
        const peak = -19 * beat.amplitude;
        const dip = 7 * beat.recovery;
        const crest = -8 * beat.recovery;
        const segments = [
            [point(-15, 0), point(-16, -3), point(-13, -3)],
            [point(-10, -3), point(-12, 0), point(-8, 0)],
            [point(-5, 0), point(-4, 3)],
            [point(-2.5, 4), point(-2, -3)],
            [point(-0.6, peak + 1)],
            [point(0, peak - 1), point(0.6, peak + 1)],
            [point(2, dip)],
            [point(3, dip + 2), point(4, dip), point(6, 4)],
            [point(10, 3), point(11, crest), point(15, crest)],
            [point(18, crest), point(20, 1), point(24, 0)]
        ];
        let commands = "";
        for (const segment of segments) {
            let points = [previous].concat(segment);
            if (points[points.length - 1][0] > limit)
                points = splitAtX(points, limit);
            const command = points.length === 2 ? " L " : points.length === 3 ? " Q " : " C ";
            commands += command + points.slice(1).map(p => p.join(" ")).join(" ");
            previous = points[points.length - 1];
            if (previous[0] >= limit)
                break;
        }
        beat.pathCache = {geometry: geometry, limit: limit, startY: startY, commands: commands, end: previous};
        result += commands;
    }
    return result + " L " + (2 * width) + " " + baseline;
}
