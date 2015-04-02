/*
Usage:

Add data-chartselector-chart-id="YOUR-CHART-ID" as buttons attibute.

  <button data-chartselector-chart-id="my-chart-1" type="button" class="btn btn-primary">Test Chart #1</button>
          ^^^^^^^^^^^^^^^^^^^^^^^^^^^ ~~~~~~~~~~~~
  
  <button data-chartselector-chart-id="my-chart-2" type="button" class="btn btn-primary">Test Chart #2</button>
          ^^^^^^^^^^^^^^^^^^^^^^^^^^^ ~~~~~~~~~~~~

Create parent <div> as chart group and child <div> as chart entity.

  <div id="my-chart-group-1" class="chartselector-group">
                             ^^^^^^^^^^^^^^^^^^^^^^^^^^^
    <div id="my-chart-1" class="chartselector-chart" data-csv-props='{...}'></div>
            ~~~~~~~~~~~~ ^^^^^^^^^^^^^^^^^^^^^^^^^^^
    <div id="my-chart-2" class="chartselector-chart" data-csv-props='{...}'></div>
            ~~~~~~~~~~~~ ^^^^^^^^^^^^^^^^^^^^^^^^^^^
  </div>
*/
(function($) {
  $(document).ready(function() {
    $("button[data-chartselector-chart-id]").click(function() {
      var chartId = $(this).attr("data-chartselector-chart-id");
      showChart(chartId);
    });
  });

  function showChart(chartId) {
    var e = $("#" + chartId);
    var p = e.closest(".chartselector-group");
    var divs = p.children(".chartselector-chart");
    divs.each(function() {
      if(this.id == chartId) {
        $(this).css("display", "block");
        $(this).css("visibility", "visible");
      } else {
        $(this).css("display", "none");
      }
    });
  }

  var cssStyleText = '\
    .chartselector-group {  \
      position: relative;   \
    }                       \
                            \
    .chartselector-chart {  \
      position: absolute;   \
      width: 100%;          \
      height: 100%;         \
    }                       \
  ';

  $("<style>").prop("type", "text/css").html(cssStyleText).appendTo("head");
})(jQuery);
