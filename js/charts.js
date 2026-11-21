/* Benchmark charts, drawn in SVG by this script, without library
 * lzbench   : compression ratio vs speed, one curve per compressor across its levels.
 *             results/index.csv lists the runs of bench/bench-lzbench.sh : results/<machine>/<date>/lzbench.csv and env.txt ;
 *             its first lzbench run is shown first.
 * pipelines : effective speeds of command lines through a link of limited speed,
 *             measured by bench/bench-pipeline.sh (csv/pipeline-i7-9700k.csv)
 */
(function () {
  "use strict";
  var SVG = "http://www.w3.org/2000/svg";

  /* ===== helpers ===== */

  var el = function (tag, attrs, parent, ns) {
    var e = ns === false ? document.createElement(tag) : document.createElementNS(SVG, tag);
    Object.keys(attrs || {}).forEach(function (k) { e.setAttribute(k, attrs[k]); });
    if (parent) parent.appendChild(e);
    return e;
  };
  var html = function (tag, attrs, parent) { return el(tag, attrs, parent, false); };
  var text = function (parent, x, y, str, attrs) {
    var t = el("text", attrs || {}, parent);
    t.setAttribute("x", x); t.setAttribute("y", y);
    t.textContent = str;
    return t;
  };

  /* speeds : 3 significant digits ; round ones (axis ticks) without trailing zeros */
  var formatSpeed = function (mbps, round) {
    var s = mbps >= 1000 ? (mbps / 1000).toPrecision(3) : mbps.toFixed(mbps >= 10 ? 0 : 1);
    return (round ? String(Number(s)) : s) + (mbps >= 1000 ? " GB/s" : " MB/s");
  };

  /* CSV with a header : rows as objects, by column name */
  var parseCsv = function (str) {
    var lines = str.replace(/\r/g, "").split("\n").filter(function (l) { return l.trim() !== ""; });
    var header = lines.shift().split(",");
    return lines.map(function (l) {
      var f = l.split(","), row = {};
      header.forEach(function (name, i) { row[name] = f[i] === undefined ? "" : f[i]; });
      return row;
    });
  };

  /* env.txt : "key : value" lines */
  var parseEnv = function (str) {
    var env = {};
    str.split("\n").forEach(function (l) {
      var m = /^(\w[\w ]*?)\s*: (.*)$/.exec(l);
      if (m) env[m[1]] = m[2];
    });
    return env;
  };

  var getText = function (url) {
    return fetch(url).then(function (r) { if (!r.ok) throw new Error(url + " : " + r.status); return r.text(); });
  };

  var linScale = function (d0, d1, r0, r1) {
    return function (v) { return r0 + (v - d0) / (d1 - d0) * (r1 - r0); };
  };
  var logScale = function (d0, d1, r0, r1) {
    var l0 = Math.log(d0), l1 = Math.log(d1);
    return function (v) { return r0 + (Math.log(v) - l0) / (l1 - l0) * (r1 - r0); };
  };

  /* a group of buttons, one selected : onSelect(i) is called when button i is clicked */
  var buttonGroup = function (labels, selected, onSelect) {
    var group = html("div", { "class": "btn-group btn-group-sm", role: "group" });
    labels.forEach(function (label, i) {
      var button = html("button", { type: "button", "class": "btn " + (i === selected ? "btn-primary" : "btn-default") }, group);
      button.innerHTML = label;
      button.addEventListener("click", function () {
        Array.prototype.forEach.call(group.children, function (b) { b.className = "btn btn-default"; });
        button.className = "btn btn-primary";
        onSelect(i);
      });
    });
    return group;
  };


  /* ===== lzbench : ratio vs speed ===== */

  /* one style per compressor family ; LZ4 is drawn last, on top */
  var families = [
    { name: "LZO",       color: "#898781", width: 1,   marker: "diamond" },
    { name: "Snappy",    color: "#898781", width: 1,   marker: "square" },
    { name: "LZF",       color: "#898781", width: 1,   marker: "triangle" },
    { name: "QuickLZ",   color: "#898781", width: 1,   marker: "triangle-down" },
    { name: "zlib",      color: "#1baf7a", width: 2,   marker: "circle" },
    { name: "zlib-ng",   color: "#1baf7a", width: 2,   marker: "circle-open", dash: "6 3" },
    { name: "Zstandard", color: "#eb6834", width: 2,   marker: "circle" },
    { name: "LZ4",       color: "#2a78d6", width: 2.5, marker: "circle", big: true }
  ];
  var familyOf = function (name) { return families.filter(function (f) { return f.name === name; })[0]; };
  var legendOrder = [ "LZ4", "Zstandard", "zlib", "zlib-ng", "LZO", "Snappy", "LZF", "QuickLZ" ];
  var hidden = {};   /* families hidden from the legend */

  var marker = function (parent, shape, x, y, size, color) {
    var s = size, attrs;
    switch (shape) {
      case "square":        attrs = { d: "M" + (x - s * 0.8) + "," + (y - s * 0.8) + "h" + 1.6 * s + "v" + 1.6 * s + "h" + -1.6 * s + "z" }; break;
      case "diamond":       attrs = { d: "M" + x + "," + (y - s * 1.1) + "l" + s * 1.1 + "," + s * 1.1 + "l" + -s * 1.1 + "," + s * 1.1 + "l" + -s * 1.1 + "," + -s * 1.1 + "z" }; break;
      case "triangle":      attrs = { d: "M" + x + "," + (y - s * 1.1) + "l" + s * 1.1 + "," + s * 1.9 + "h" + -s * 2.2 + "z" }; break;
      case "triangle-down": attrs = { d: "M" + x + "," + (y + s * 1.1) + "l" + s * 1.1 + "," + -s * 1.9 + "h" + -s * 2.2 + "z" }; break;
      default:              return el("circle", { cx: x, cy: y, r: s, fill: shape === "circle-open" ? "#fff" : color, stroke: shape === "circle-open" ? color : "#fff",
                                                  "stroke-width": shape === "circle-open" ? 1.5 : 1 }, parent);
    }
    attrs.fill = color; attrs.stroke = "#fff"; attrs["stroke-width"] = 1;
    return el("path", attrs, parent);
  };

  /* ticks of a log scale : powers of 10, and 2 and 5 times them when there is room */
  var logTicks = function (d0, d1, pxPerDecade) {
    var ticks = [], mults = pxPerDecade > 200 ? [ 1, 2, 5 ] : [ 1 ];
    for (var p = Math.floor(Math.log(d0) / Math.LN10); Math.pow(10, p) <= d1; p++) {
      mults.forEach(function (m) { var v = m * Math.pow(10, p); if (v >= d0 && v <= d1) ticks.push(v); });
    }
    return ticks;
  };

  /* chart of one speed (compression or decompression) vs ratio, for the rows of one run */
  var ScatterChart = function (box, metric, onHover) {
    this.box = box; this.metric = metric; this.onHover = onHover;
  };
  ScatterChart.prototype.draw = function (rows, all) {   /* all : rows of all runs, which set the axes */
    var self = this, key = this.metric === "compression" ? "cSpeed" : "dSpeed";
    var box = this.box;
    box.innerHTML = "";
    var width = box.clientWidth, height = Math.max(300, Math.min(460, Math.round(width * 0.72)));
    var m = { left: 46, right: 14, top: 12, bottom: 44 };
    var svg = el("svg", { width: width, height: height, viewBox: "0 0 " + width + " " + height, role: "img",
                          "aria-label": "Compression ratio vs " + this.metric + " speed, for each compressor and level" }, box);

    var memcpy = rows.filter(function (r) { return r.family === "memcpy"; })[0];
    var points = rows.filter(function (r) { return r.family !== "memcpy" && familyOf(r.family); });
    var speeds = all.map(function (r) { return r[key]; });
    var xMin = Math.min.apply(null, speeds) / 1.4, xMax = Math.max.apply(null, speeds) * 1.25;
    var yMax = Math.ceil((Math.max.apply(null, all.map(function (r) { return r.ratio; })) + 0.1) * 2) / 2;
    var x = logScale(xMin, xMax, m.left, width - m.right), y = linScale(1, yMax, height - m.bottom, m.top);

    /* grid and axes */
    var grid = el("g", { "class": "chart-grid" }, svg);
    var pxPerDecade = (width - m.left - m.right) / (Math.log(xMax / xMin) / Math.LN10);
    logTicks(xMin, xMax, pxPerDecade).forEach(function (v) {
      el("line", { x1: x(v), x2: x(v), y1: m.top, y2: height - m.bottom }, grid);
      text(grid, x(v), height - m.bottom + 16, formatSpeed(v, true), { "class": "chart-tick", "text-anchor": "middle" });
    });
    for (var r = 1; r <= yMax + 1e-9; r += 0.5) {
      el("line", { x1: m.left, x2: width - m.right, y1: y(r), y2: y(r) }, grid);
      text(grid, m.left - 6, y(r) + 4, r.toFixed(1), { "class": "chart-tick", "text-anchor": "end" });
    }
    el("line", { "class": "chart-axis", x1: m.left, x2: width - m.right, y1: height - m.bottom, y2: height - m.bottom }, svg);
    text(svg, (m.left + width - m.right) / 2, height - 8, (this.metric === "compression" ? "Compression" : "Decompression") + " speed (log scale)",
         { "class": "chart-axis-title", "text-anchor": "middle" });
    text(svg, 0, 0, "Compression ratio", { "class": "chart-axis-title", "text-anchor": "middle",
                                           transform: "translate(12," + (m.top + height - m.bottom) / 2 + ") rotate(-90)" });

    /* memcpy : the speed of a plain memory copy */
    if (memcpy) {
      el("line", { "class": "chart-ref", x1: x(memcpy[key]), x2: x(memcpy[key]), y1: m.top, y2: height - m.bottom }, svg);
      text(svg, x(memcpy[key]) - 4, m.top + 10, "memcpy", { "class": "chart-ref-label", "text-anchor": "end" });
    }

    /* one curve per family, through its levels */
    this.marks = [];
    families.forEach(function (f) {
      if (hidden[f.name]) return;
      var pts = points.filter(function (p) { return p.family === f.name; }).sort(function (a, b) { return a.level - b.level; });
      if (!pts.length) return;
      var g = el("g", {}, svg);
      if (pts.length > 1) {
        el("polyline", { points: pts.map(function (p) { return x(p[key]).toFixed(1) + "," + y(p.ratio).toFixed(1); }).join(" "),
                         fill: "none", stroke: f.color, "stroke-width": f.width, "stroke-dasharray": f.dash || "none",
                         "stroke-linejoin": "round" }, g);
      }
      pts.forEach(function (p) {
        marker(g, f.marker, x(p[key]), y(p.ratio), f.big ? 4.5 : 3.5, f.color);
        self.marks.push({ row: p, x: x(p[key]), y: y(p.ratio) });
      });
    });

    /* hover : the nearest point within reach, highlighted in all charts */
    this.ring = el("circle", { "class": "chart-ring", r: 8, cx: -99, cy: -99 }, svg);
    this.tip = html("div", { "class": "chart-tip" }, box);
    var overlay = el("rect", { x: 0, y: 0, width: width, height: height, fill: "transparent" }, svg);
    var nearest = function (evt) {
      var rect = svg.getBoundingClientRect(), px = evt.clientX - rect.left, py = evt.clientY - rect.top, best = null, bestD = 24 * 24;
      self.marks.forEach(function (mk) {
        var d = (mk.x - px) * (mk.x - px) + (mk.y - py) * (mk.y - py);
        if (d < bestD) { bestD = d; best = mk; }
      });
      return best;
    };
    var move = function (evt) { var mk = nearest(evt); self.onHover(mk ? mk.row : null, self); };
    overlay.addEventListener("pointermove", move);
    overlay.addEventListener("pointerdown", move);
    overlay.addEventListener("pointerleave", function () { self.onHover(null, self); });
  };
  ScatterChart.prototype.highlight = function (row, withTip) {
    var mk = row && this.marks.filter(function (k) { return k.row === row; })[0];
    this.ring.setAttribute("cx", mk ? mk.x : -99);
    this.ring.setAttribute("cy", mk ? mk.y : -99);
    if (!mk || !withTip) { this.tip.style.display = "none"; return; }
    this.tip.innerHTML = "<strong>" + row.name + "</strong> <span class=\"chart-tip-version\">" + row.family + " " + row.version + "</span><br/>" +
                         "ratio " + row.ratio.toFixed(3) + "<br/>compression " + formatSpeed(row.cSpeed) + "<br/>decompression " + formatSpeed(row.dSpeed);
    this.tip.style.display = "block";
    var left = mk.x + 12, top = mk.y - 12;
    if (left + this.tip.offsetWidth > this.box.clientWidth) left = mk.x - 12 - this.tip.offsetWidth;
    if (top + this.tip.offsetHeight > this.box.clientHeight) top = this.box.clientHeight - this.tip.offsetHeight;
    this.tip.style.left = Math.max(0, left) + "px";
    this.tip.style.top = Math.max(0, top) + "px";
  };

  var lzbench = { runs: [], rows: [], charts: [] };   /* runs : { machine, date, dir, rows, env } */

  var drawLegend = function (place) {
    place.innerHTML = "";
    legendOrder.forEach(function (name) {
      var f = familyOf(name);
      if (!lzbench.rows.some(function (r) { return r.family === name; })) return;
      var b = html("button", { type: "button", "class": "chart-legend-item" + (hidden[name] ? " off" : ""), "aria-pressed": hidden[name] ? "false" : "true",
                               title: "show or hide " + name }, place);
      var sw = el("svg", { width: 26, height: 12, "aria-hidden": "true" }, b);
      el("line", { x1: 1, x2: 25, y1: 6, y2: 6, stroke: f.color, "stroke-width": f.width, "stroke-dasharray": f.dash || "none" }, sw);
      marker(sw, f.marker, 13, 6, 3.5, f.color);
      b.appendChild(document.createTextNode(name));
      b.addEventListener("click", function () {
        hidden[name] = !hidden[name];
        drawLegend(place);
        drawLzbench();
      });
    });
  };

  /* the run's environment, from env.txt */
  var describeRun = function (env) {
    var compiler = (env.compilers || "").split(" ; ")[0];
    compiler = /clang/.test(compiler) ? "clang " + (/clang version ([\d.]+)/.exec(compiler) || [ "", "" ])[1]
                                      : "gcc " + (/\) (\d+(\.\d+)*)/.exec(compiler) || [ "", "" ])[1];
    var cpu = (env.cpu || "").replace(/\((R|TM)\)/g, "").replace(/ CPU @ [\d.]+GHz/, "").replace(/, \d+ threads$/, "");
    var system = (env.system || "").split(", kernel")[0];
    var lzb = (env.lzbench || "").split(",")[0];
    var jcc = /^lz4 only : (\S+)/.exec(env.jcc || "");
    return "Measured on " + env.date + ", on " + cpu + ", " + system + ", with " + lzb + " compiled by " + compiler +
           (jcc ? " (lz4 with <code>" + jcc[1] + "</code>, as its own build)" : "") +
           ". Single thread, Silesia corpus as a tar, fastest of iterations lasting at least 1 s (compression) and 2 s (decompression)." +
           " Script : <a href=\"https://github.com/lz4/lz4/blob/gh-pages/bench/bench-lzbench.sh\">bench-lzbench.sh</a>.";
  };

  var drawTable = function (place) {
    var rows = lzbench.rows.filter(function (r) { return r.family !== "memcpy"; });
    var t = "<table class=\"table table-condensed chart-table\"><thead><tr><th>Compressor</th><th>Version</th><th>Ratio</th>" +
            "<th>Compression</th><th>Decompression</th></tr></thead><tbody>";
    rows.forEach(function (r) {
      t += "<tr><td><code>" + r.name + "</code></td><td>" + r.version + "</td><td>" + r.ratio.toFixed(3) + "</td><td>" +
           formatSpeed(r.cSpeed) + "</td><td>" + formatSpeed(r.dSpeed) + "</td></tr>";
    });
    place.innerHTML = t + "</tbody></table>";
  };

  var drawLzbench = function () {
    var all = [];
    lzbench.runs.forEach(function (run) { all = all.concat(run.rows); });
    lzbench.charts.forEach(function (c) { c.draw(lzbench.rows, all); });
  };

  var loadRun = function (run) {
    return Promise.all([ getText(run.dir + "/lzbench.csv"), getText(run.dir + "/env.txt").catch(function () { return ""; }) ])
      .then(function (replies) {
        run.rows = parseCsv(replies[0]).map(function (r) {
          return { family: r.family, name: r.name, level: Number(r.level), version: r.version, ratio: Number(r.ratio),
                   cSpeed: Number(r.compression_MBps), dSpeed: Number(r.decompression_MBps) };
        });
        run.env = replies[1] ? parseEnv(replies[1]) : null;
      });
  };

  var showRun = function (run, root) {
    lzbench.rows = run.rows;
    drawLegend(root.querySelector(".chart-legend"));
    drawLzbench();
    root.querySelector(".chart-env").innerHTML = run.env ? describeRun(run.env) : "";
    drawTable(root.querySelector(".chart-table-place"));
  };

  var setupLzbench = function (root) {
    root.innerHTML =
      "<div class=\"chart-controls\"><div class=\"chart-machine\"></div><div class=\"chart-legend\"></div></div>" +
      "<div class=\"row\">" +
      "<div class=\"col-md-6\"><h4 class=\"chart-title\">Ratio vs decompression speed <small>farther up and right is better</small></h4>" +
      "<div class=\"chart-box\" data-metric=\"decompression\"></div></div>" +
      "<div class=\"col-md-6\"><h4 class=\"chart-title\">Ratio vs compression speed <small>farther up and right is better</small></h4>" +
      "<div class=\"chart-box\" data-metric=\"compression\"></div></div>" +
      "</div>" +
      "<p class=\"chart-env\"></p>" +
      "<details><summary>All results</summary><div class=\"chart-table-place\"></div></details>";
    var onHover = function (row, from) {
      lzbench.charts.forEach(function (c) { c.highlight(row, c === from); });
    };
    Array.prototype.forEach.call(root.querySelectorAll(".chart-box"), function (box) {
      lzbench.charts.push(new ScatterChart(box, box.getAttribute("data-metric"), onHover));
    });
    return getText("results/index.csv").then(function (str) {
      lzbench.runs = parseCsv(str).filter(function (r) { return r.benchmark === "lzbench"; })
                                  .map(function (r) { return { machine: r.machine, date: r.date, dir: "results/" + r.dir }; });
      if (lzbench.runs.length > 1) {
        var select = html("select", { "class": "form-control input-sm", "aria-label": "machine" }, root.querySelector(".chart-machine"));
        lzbench.runs.forEach(function (run, i) {
          html("option", { value: i }, select).textContent = run.machine + " (" + run.date + ")";
        });
        select.addEventListener("change", function () { showRun(lzbench.runs[select.value], root); });
      }
      return Promise.all(lzbench.runs.map(loadRun));
    }).then(function () { showRun(lzbench.runs[0], root); });
  };


  /* ===== pipelines : measured effective speeds, for a selected link =====
   * Transfers : default settings, or a single compression thread, whose command lines end with -T1 or -p 1 */

  var pipeline = [];   /* { scenario, link, linkSpeed, compressor, speed, version } */
  var scenarios = { read: { defaultLink: "SATA SSD" }, transfer: { defaultLink: "5 GbE", singleThread: false } };

  var isSingleThreadVariant = function (command) { return / (-T1|-p 1)$/.test(command); };
  var hasSingleThreadVariant = function (command) {
    return pipeline.some(function (p) { return isSingleThreadVariant(p.compressor) && p.compressor.replace(/ (-T1|-p 1)$/, "") === command; });
  };

  /* ticks from 0 to beyond max, at most n steps of a round size (1, 2, 2.5 or 5 x 10^k) */
  var linearTicks = function (max, n) {
    var base = Math.pow(10, Math.floor(Math.log(max / n) / Math.LN10));
    var multipliers = [ 1, 2, 2.5, 5, 10 ], step = 0, values = [ 0 ];
    for (var i = 0; i < multipliers.length && !step; i++) {
      if (max / (base * multipliers[i]) <= n) step = base * multipliers[i];
    }
    while (values[values.length - 1] < max) values.push(values[values.length - 1] + step);
    return values;
  };

  var drawPipeline = function (scenarioName) {
    var scenario = scenarios[scenarioName];
    var rows = pipeline.filter(function (p) {
      if (p.scenario !== scenarioName || p.link !== scenario.selected) return false;
      return scenario.singleThread ? (isSingleThreadVariant(p.compressor) || !hasSingleThreadVariant(p.compressor))
                                   : !isSingleThreadVariant(p.compressor);
    }).sort(function (a, b) { return b.speed - a.speed; });
    if (!rows.length) return;
    var box = document.querySelector(".benchmark-chart[data-scenario=" + scenarioName + "]");
    var title = document.querySelector(".benchmark-title[data-scenario=" + scenarioName + "]");
    title.innerHTML = scenario.selected + " (" + formatSpeed(rows[0].linkSpeed, true) + ") <small>measured speed, MB/s of original data : higher is better</small>";
    box.innerHTML = "";
    var barH = 22, gap = 7, m = { top: 4, bottom: 26, right: 70 };
    var width = box.clientWidth, height = m.top + rows.length * (barH + gap) + m.bottom;
    var svg = el("svg", { width: width, height: height, viewBox: "0 0 " + width + " " + height, role: "img",
                          "aria-label": "measured speed of each command line" }, box);
    /* labels first : the longest one sets the left margin */
    var labels = rows.map(function (p) { return text(svg, 0, 0, p.compressor, { "class": "chart-label", "text-anchor": "end" }); });
    var left = 8 + Math.max.apply(null, labels.map(function (t) { return t.getComputedTextLength(); }));
    var max = Math.max.apply(null, rows.map(function (p) { return p.speed; }));
    var ticks = linearTicks(max, Math.max(2, Math.floor((width - left - m.right) / 80)));
    var x = linScale(0, ticks[ticks.length - 1], left, width - m.right);
    var grid = el("g", { "class": "chart-grid" }, svg);
    ticks.forEach(function (v) {
      el("line", { x1: x(v), x2: x(v), y1: m.top, y2: height - m.bottom }, grid);
      text(grid, x(v), height - m.bottom + 16, v === 0 ? "0" : formatSpeed(v, true), { "class": "chart-tick", "text-anchor": "middle" });
    });
    rows.forEach(function (p, i) {
      var top = m.top + i * (barH + gap), color = p.compressor === "no compression" ? "#c3c2b7" : /^lz4/.test(p.compressor) ? "#2a78d6" : "#a9bfe6";
      labels[i].setAttribute("x", left - 8); labels[i].setAttribute("y", top + barH / 2 + 4);
      var bar = el("rect", { x: left, y: top, width: Math.max(1, x(p.speed) - left), height: barH, rx: 3, fill: color }, svg);
      el("title", {}, bar).textContent = p.compressor + " : " + formatSpeed(p.speed);
      text(svg, x(p.speed) + 5, top + barH / 2 + 4, formatSpeed(p.speed), { "class": "chart-value" });
    });
    el("line", { "class": "chart-axis", x1: left, x2: left, y1: m.top, y2: height - m.bottom }, svg);
  };

  /* versions of the programs, as measured : "lz4 1.11.1, zstd 1.5.7, ..." */
  var showVersions = function (scenarioName) {
    var versions = {}, list = [];
    pipeline.forEach(function (p) {
      if (p.scenario === scenarioName && p.version) versions[p.compressor.split(" ")[0]] = p.version.replace(/^v/, "");
    });
    Object.keys(versions).forEach(function (prog) { list.push(prog + " " + versions[prog]); });
    if (list.length) document.querySelector(".benchmark-versions[data-scenario=" + scenarioName + "]").innerHTML = "Versions : " + list.join(", ") + ".";
  };

  var setupPipeline = function () {
    return getText("csv/pipeline-i7-9700k.csv").then(function (str) {
      pipeline = parseCsv(str).map(function (r) {
        return { scenario: r.scenario, link: r.link, linkSpeed: Number(r.link_MBps), compressor: r.compressor,
                 speed: Number(r.effective_MBps), version: r.version || "" };
      });
      Object.keys(scenarios).forEach(function (scenarioName) {
        var scenario = scenarios[scenarioName];
        var place = document.querySelector(".benchmark-links[data-scenario=" + scenarioName + "]");
        showVersions(scenarioName);
        var links = pipeline.filter(function (p) { return p.scenario === scenarioName && p.compressor === "no compression"; });
        scenario.selected = scenario.defaultLink;
        place.appendChild(buttonGroup(
          links.map(function (link) { return link.link + "<br/><small>" + formatSpeed(link.linkSpeed, true) + "</small>"; }),
          links.map(function (link) { return link.link; }).indexOf(scenario.selected),
          function (i) { scenario.selected = links[i].link; drawPipeline(scenarioName); }));
        if (scenario.singleThread !== undefined) {
          var modes = buttonGroup([ "Default settings<br/><small>compression</small>", "Single thread<br/><small>compression</small>" ],
                                  scenario.singleThread ? 1 : 0,
                                  function (i) { scenario.singleThread = i === 1; drawPipeline(scenarioName); });
          modes.style.marginLeft = "1em";
          place.appendChild(modes);
        }
        drawPipeline(scenarioName);
      });
    });
  };


  /* ===== setup ===== */

  /* charts are drawn at the width of their container : redraw them when the width changes.
   * Not the height : mobile browsers change it when the address bar hides, while scrolling.
   * Hidden tabs have no width : their charts are drawn when the tab is shown. */
  var drawAll = function () {
    drawLzbench();
    Object.keys(scenarios).forEach(function (s) { if (scenarios[s].selected) drawPipeline(s); });
  };
  var redrawOnResize = function () {
    var width = document.documentElement.clientWidth, timer;
    window.addEventListener("resize", function () {
      if (document.documentElement.clientWidth === width) return;
      width = document.documentElement.clientWidth;
      clearTimeout(timer);
      timer = setTimeout(drawAll, 200);
    });
    document.addEventListener("click", function (evt) {
      if (evt.target.closest && evt.target.closest("[data-toggle=tab]")) setTimeout(drawAll, 0);
    });
  };

  var root = document.getElementById("lzbench-charts");
  Promise.all([ root ? setupLzbench(root) : null, setupPipeline() ]).then(redrawOnResize);
})();
