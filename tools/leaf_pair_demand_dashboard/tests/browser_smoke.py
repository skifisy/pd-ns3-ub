"""Optional real-browser checks against 64 pairs of clearly synthetic data."""
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

from playwright.sync_api import sync_playwright

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from fixtures import make_case
from prepare import parser, prepare


def run():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        make_case(root / "case", full=True, count=1024)
        with redirect_stdout(io.StringIO()):
            cache = prepare(parser().parse_args(["--case",str(root / "case"),"--out-dir",str(root/"out"),
                                                 "--throughput-scale","100000"]))
        with socket.socket() as sock:
            sock.bind(("127.0.0.1",0))
            port = sock.getsockname()[1]
        process = subprocess.Popen([sys.executable,str(HERE/"server.py"),"--data",str(cache),"--port",str(port)],
                                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        try:
            url = f"http://127.0.0.1:{port}"
            for _ in range(100):
                try:
                    with urllib.request.urlopen(url+"/api/bootstrap",timeout=1):
                        break
                except OSError:
                    if process.poll() is not None:
                        raise RuntimeError("Server exited before browser checks")
                    time.sleep(.05)
            else:
                raise RuntimeError("Server did not start")
            with sync_playwright() as playwright:
                executable = os.environ.get("PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH") or shutil.which("chromium")
                browser = playwright.chromium.launch(headless=True, executable_path=executable)
                page = browser.new_page(viewport={"width":1440,"height":1000})
                errors = []
                page.on("pageerror",lambda error: errors.append(str(error)))
                page.goto(url)
                page.wait_for_function("document.getElementById('status').textContent.startsWith('已显示')")
                assert page.locator(".chart-card").count() == 64
                assert page.locator("#statistics tr").count() == 128
                page.wait_for_function("state.charts.size>0")
                assert page.evaluate("state.charts.size") < 64
                # Full view and zoomed view must report exactly the same raw peak.
                peak = page.evaluate("Math.max(...state.data.directions['0'].bins.map(r=>r[3]))")
                assert peak == 240
                page.locator("#start").fill("0.65")
                page.locator("#end").fill("0.651")
                page.get_by_role("button",name="应用范围").click()
                page.wait_for_function("state.data.start===.65 && state.data.end===.651")
                assert page.evaluate("Math.max(...state.data.directions['0'].bins.map(r=>r[3]))") == peak
                assert page.evaluate("[...state.charts.values()].every(c=>c.getOption().xAxis[0].min===.65)")
                # Metric, direction, capacity switch, and sorting are independent of time selection.
                page.locator("#metric").select_option("wire")
                page.wait_for_function("state.data.metric==='wire'")
                page.locator("#direction").select_option("reverse")
                assert page.locator("#statistics tr").count() == 64
                page.locator("#showCapacity").uncheck()
                assert page.evaluate("[...state.charts.values()].every(c=>c.getOption().series.every(s=>!s.id.startsWith('capacity-')))")
                page.locator("#direction").select_option("both")
                page.locator("#showCapacity").check()
                page.locator("#sort").select_option("peak")
                page.get_by_role("button",name="全程",exact=True).click()
                page.wait_for_function("state.data.start===0 && state.data.end===state.info.end")
                # Wheel in a small chart updates the GLOBAL range and every visible x axis.
                plot=page.locator(".chart").first
                plot.scroll_into_view_if_needed()
                box=plot.bounding_box()
                page.mouse.move(box["x"]+box["width"]*.6,box["y"]+80)
                page.mouse.wheel(0,-200)
                page.wait_for_function("state.data.end-state.data.start < state.info.end-state.info.start")
                width=page.evaluate("state.range[1]-state.range[0]")
                # Start away from the left boundary so drag movement can be measured.
                page.evaluate("setRange(.2,.7,true)")
                page.wait_for_function("state.data.start===.2 && state.data.end===.7")
                box=plot.bounding_box()
                page.mouse.move(box["x"]+box["width"]*.55,box["y"]+80)
                page.mouse.down()
                page.mouse.move(box["x"]+box["width"]*.4,box["y"]+80,steps=5)
                page.mouse.up()
                page.wait_for_function("state.data.start>.2")
                assert abs(page.evaluate("state.range[1]-state.range[0]")-.5)<1e-9
                # Unified crosshair actions must terminate without recursive chart events.
                page.mouse.move(box["x"]+box["width"]*.7,box["y"]+100)
                # Change the overview slider programmatically through its actual ECharts event.
                page.evaluate("state.overview.dispatchAction({type:'dataZoom',startValue:.3,endValue:.8})")
                page.wait_for_function("state.data.start===.3 && state.data.end===.8")
                assert page.evaluate("[...state.charts.values()].every(c=>Math.abs(c.getOption().xAxis[0].min-.3)<1e-9)")
                # Screenshot path can be passed explicitly for local visual inspection.
                if len(sys.argv)>1:
                    page.evaluate("window.scrollTo(0,0)")
                    page.screenshot(path=sys.argv[1],full_page=False)
                assert not errors, errors
                browser.close()
                print(f"Browser checks passed: 64 pairs, 128 directions; wheel span {width:.6f}s")
        finally:
            process.terminate()
            process.wait(timeout=10)


if __name__ == "__main__":
    run()
