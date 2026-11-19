/* Benchmark charts
 * lzbench    : compression ratio vs speed, and speeds and ratio (csv/benchmark-silesia.csv, from the README)
 * pipelines  : effective speeds of command lines through a link of limited speed,
 *              measured by bench/bench-pipeline.sh (csv/pipeline-i7-9700k.csv)
 */
(function () {
  var codecs = [];     /* { name, ratio, cSpeed, dSpeed } */
  var pipeline = [];   /* { scenario, link, linkSpeed, compressor, speed } */

  var isLZ4 = function (name) { return /^lz4/i.test(name); };
  var barColor = function (name) { return isLZ4(name) ? "#3366cc" : "#a9bfe6"; };
  var shortName = function (name) { return name.replace(/ \(v[\d.]+\)/, "").replace(/ v?\d+(\.\d+)+[a-z0-9]*/, ""); };

  var formatSpeed = function (mbps) {
    if (mbps >= 1000) return Number((mbps / 1000).toPrecision(3)) + " GB/s";
    return Number(mbps.toFixed(mbps >= 10 ? 0 : 1)) + " MB/s";
  };

  var parseCsv = function (text) {
    var lines = text.replace(/\r/g, "").split("\n");
    var rows = [];
    for (var i = 1; i < lines.length; i++) {
      if (lines[i].trim() !== "") rows.push(lines[i].split(","));
    }
    return rows;
  };

  var chartEl = function (name) { return document.querySelector(".bench-chart[data-chart=" + name + "]"); };

  /* lzbench : ratio vs speed */
  var speedTicks = function (values) {
    return values.map(function (v) { return { v: v, f: formatSpeed(v) }; });
  };

  /* ticks from 0 to beyond max, at most n steps of a round size (1, 2, 2.5 or 5 x 10^k), labelled with units */
  var linearSpeedTicks = function (max, n) {
    var base = Math.pow(10, Math.floor(Math.log(max / n) / Math.LN10));
    var multipliers = [ 1, 2, 2.5, 5, 10 ], step = 0, values = [ 0 ];
    for (var i = 0; i < multipliers.length && !step; i++) {
      if (max / (base * multipliers[i]) <= n) step = base * multipliers[i];
    }
    while (values[values.length - 1] < max) values.push(values[values.length - 1] + step);
    return speedTicks(values).map(function (t) { if (t.v === 0) t.f = "0"; return t; });
  };

  var maxOf = function (rows) {
    var max = 0;
    rows.forEach(function (r) { if (r[1] > max) max = r[1]; });
    return max;
  };

  var drawScatter = function (key, label, hAxis) {
    var data = new google.visualization.DataTable();
    data.addColumn("number", label);
    data.addColumn("number", "Compression ratio");
    data.addColumn({ type: "string", role: "style" });
    data.addColumn({ type: "string", role: "annotation" });
    data.addColumn({ type: "string", role: "tooltip" });
    codecs.forEach(function (c) {
      data.addRow([ c[key], c.ratio,
                    "point { size: " + (isLZ4(c.name) ? 9 : 6) + "; fill-color: " + barColor(c.name) + "; }",
                    shortName(c.name),
                    c.name + "\nratio " + c.ratio + ", " + label.toLowerCase() + " " + formatSpeed(c[key]) ]);
    });
    new google.visualization.ScatterChart(chartEl("scatter-" + label.split(" ")[0].toLowerCase())).draw(data, {
      title: label + " vs compression ratio : farther up and right is better",
      legend: { position: "none" },
      hAxis: hAxis,
      vAxis: { title: "Compression ratio" },
      chartArea: { left: "12%", width: "80%", height: "72%" },
      annotations: { textStyle: { fontSize: 10, color: "#555555" } }
    });
  };

  /* lzbench : one chart per measurement */
  var drawBars = function (chart, key, title, format) {
    var rows = [];
    codecs.forEach(function (c) { rows.push([ shortName(c.name), c[key], "color: " + barColor(c.name), format(c[key]), c.name ]); });
    rows.sort(function (a, b) { return b[1] - a[1]; });
    var data = new google.visualization.DataTable();
    data.addColumn("string", "Compressor");
    data.addColumn("number", title);
    data.addColumn({ type: "string", role: "style" });
    data.addColumn({ type: "string", role: "annotation" });
    data.addColumn({ type: "string", role: "tooltip" });
    data.addRows(rows);
    new google.visualization.BarChart(chartEl(chart)).draw(data, {
      title: title,
      legend: { position: "none" },
      chartArea: { left: "32%", width: "50%", height: "85%" },
      hAxis: key === "ratio" ? { minValue: 0 } : { minValue: 0, ticks: linearSpeedTicks(maxOf(rows), 2) },
      annotations: { alwaysOutside: true, textStyle: { fontSize: 10 } }
    });
  };

  /* pipelines : measured effective speeds, for a selected link.
   * Transfers : default settings, or a single compression thread, whose command lines end with -T1 or -p 1 */
  var scenarios = { read: { defaultLink: "SATA SSD" }, transfer: { defaultLink: "5 GbE", singleThread: false } };

  var isSingleThreadVariant = function (command) { return / (-T1|-p 1)$/.test(command); };

  var hasSingleThreadVariant = function (command) {
    var found = false;
    pipeline.forEach(function (p) {
      if (isSingleThreadVariant(p.compressor) && p.compressor.replace(/ (-T1|-p 1)$/, "") === command) found = true;
    });
    return found;
  };

  var drawPipeline = function (scenarioName) {
    var scenario = scenarios[scenarioName];
    var rows = [], linkSpeed = 0;
    pipeline.forEach(function (p) {
      if (p.scenario !== scenarioName || p.link !== scenario.selected) return;
      if (scenario.singleThread ? (!isSingleThreadVariant(p.compressor) && hasSingleThreadVariant(p.compressor))
                                : isSingleThreadVariant(p.compressor)) return;
      linkSpeed = p.linkSpeed;
      rows.push([ p.compressor, p.speed, p.compressor === "no compression" ? "color: #888888" : "color: " + barColor(p.compressor),
                  formatSpeed(p.speed), p.compressor + "\n" + formatSpeed(p.speed) ]);
    });
    rows.sort(function (a, b) { return b[1] - a[1]; });
    var data = new google.visualization.DataTable();
    data.addColumn("string", "Command line");
    data.addColumn("number", "Speed");
    data.addColumn({ type: "string", role: "style" });
    data.addColumn({ type: "string", role: "annotation" });
    data.addColumn({ type: "string", role: "tooltip" });
    data.addRows(rows);
    var el = document.querySelector(".benchmark-chart[data-scenario=" + scenarioName + "]");
    var link = scenario.selected + " (" + formatSpeed(linkSpeed) + ")", unit = "measured speed, MB/s of original data";
    var narrow = el.offsetWidth < 500;   /* the title has 2 lines at most : on small screens, the unit goes below the axis */
    new google.visualization.BarChart(el).draw(data, {
      title: (narrow ? link : link + " : " + unit) + "\nHIGHER IS BETTER",
      legend: { position: "none" },
      chartArea: { left: "22%", width: "62%", height: "82%" },
      hAxis: { minValue: 0, ticks: linearSpeedTicks(maxOf(rows), Math.max(3, Math.floor(el.offsetWidth * 0.62 / 75))),
               title: narrow ? unit : null },
      annotations: { alwaysOutside: true, textStyle: { fontSize: 11 } }
    });
  };

  /* versions of the programs, as measured : "lz4 v1.11.1, zstd v1.5.7, ..." */
  var showVersions = function (scenarioName) {
    var versions = {}, list = [];
    pipeline.forEach(function (p) {
      if (p.scenario === scenarioName && p.version) versions[p.compressor.split(" ")[0]] = p.version.replace(/^v/, "");
    });
    Object.keys(versions).forEach(function (prog) { list.push(prog + " " + versions[prog]); });
    if (list.length) document.querySelector(".benchmark-versions[data-scenario=" + scenarioName + "]").innerHTML = "Versions : " + list.join(", ") + ".";
  };

  /* a group of buttons, one selected : onSelect(i) is called when button i is clicked */
  var buttonGroup = function (labels, selected, onSelect) {
    var group = document.createElement("div");
    group.className = "btn-group btn-group-sm";
    group.setAttribute("role", "group");
    labels.forEach(function (label, i) {
      var button = document.createElement("button");
      button.type = "button";
      button.className = "btn " + (i === selected ? "btn-primary" : "btn-default");
      button.innerHTML = label;
      button.addEventListener("click", function () {
        Array.prototype.forEach.call(group.children, function (b) { b.className = "btn btn-default"; });
        button.className = "btn btn-primary";
        onSelect(i);
      });
      group.appendChild(button);
    });
    return group;
  };

  var setupPipeline = function () {
    Object.keys(scenarios).forEach(function (scenarioName) {
      var scenario = scenarios[scenarioName];
      var place = document.querySelector(".benchmark-links[data-scenario=" + scenarioName + "]");
      showVersions(scenarioName);
      var links = pipeline.filter(function (p) { return p.scenario === scenarioName && p.compressor === "no compression"; });
      scenario.selected = scenario.defaultLink;
      place.appendChild(buttonGroup(
        links.map(function (link) { return link.link + "<br/><small>" + formatSpeed(link.linkSpeed) + "</small>"; }),
        links.map(function (link) { return link.link; }).indexOf(scenario.selected),
        function (i) { scenario.selected = links[i].link; drawPipeline(scenarioName); }));
      if (scenario.singleThread === undefined) return;
      var modes = buttonGroup([ "Default settings<br/><small>compression</small>", "Single thread<br/><small>compression</small>" ],
                              scenario.singleThread ? 1 : 0,
                              function (i) { scenario.singleThread = i === 1; drawPipeline(scenarioName); });
      modes.style.marginLeft = "1em";
      place.appendChild(modes);
    });
  };

  var drawAll = function () {
    drawScatter("dSpeed", "Decompression speed",
                { title: "Decompression speed (log scale)", logScale: true, ticks: speedTicks([300, 500, 1000, 2000, 5000, 10000]) });
    drawScatter("cSpeed", "Compression speed",
                { title: "Compression speed", minValue: 0, ticks: speedTicks([0, 200, 400, 600, 800]) });
    drawBars("bars-decompression", "dSpeed", "Decompression speed", formatSpeed);
    drawBars("bars-compression", "cSpeed", "Compression speed", formatSpeed);
    drawBars("bars-ratio", "ratio", "Compression ratio", function (r) { return r.toFixed(3); });
    drawPipeline("read");
    drawPipeline("transfer");
  };

  /* charts are drawn at the width of their container : redraw them when the width changes.
   * Not the height : mobile browsers change it when the address bar hides, while scrolling. */
  var redrawOnResize = function () {
    var width = document.documentElement.clientWidth, timer;
    window.addEventListener("resize", function () {
      if (document.documentElement.clientWidth === width) return;
      width = document.documentElement.clientWidth;
      clearTimeout(timer);
      timer = setTimeout(drawAll, 200);
    });
  };

  var setup = function () {
    setupPipeline();
    drawAll();
    redrawOnResize();
  };

  google.charts.load("current", { packages: ["corechart"] });
  google.charts.setOnLoadCallback(function () {
    var getText = function (url) {
      return fetch(url).then(function (r) { if (!r.ok) throw new Error(url + " : " + r.status); return r.text(); });
    };
    Promise.all([ getText("csv/benchmark-silesia.csv"), getText("csv/pipeline-i7-9700k.csv") ])
      .then(function (replies) {
        parseCsv(replies[0]).forEach(function (f) {
          codecs.push({ name: f[0], ratio: Number(f[1]), cSpeed: Number(f[2]), dSpeed: Number(f[3]) });
        });
        /* pipeline.csv, as written by bench-pipeline.sh : columns found by name */
        var header = replies[1].split("\n")[0].trim().split(",");
        var col = function (name) { return header.indexOf(name); };
        parseCsv(replies[1]).forEach(function (f) {
          pipeline.push({ scenario: f[col("scenario")], link: f[col("link")], linkSpeed: Number(f[col("link_MBps")]),
                          compressor: f[col("compressor")], speed: Number(f[col("effective_MBps")]),
                          version: col("version") >= 0 ? f[col("version")] : "" });
        });
        setup();
      });
  });
})();
