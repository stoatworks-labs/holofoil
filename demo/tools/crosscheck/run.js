// The page's half of the crosscheck: fetch the cases and the harness's card
// frames from crosscheck.py's server, render each case through
// window.holofoilDemo.renderCase, and post the picture back, with the page's
// control table and its colour table's format. crosscheck.py compares;
// nothing is judged here.
const post = (path, body) => fetch(path, { method: 'POST', body });

async function main() {
  for (let i = 0; i < 200 && !window.holofoilDemo; i += 1) await new Promise((r) => setTimeout(r, 50));
  if (!window.holofoilDemo) throw new Error('window.holofoilDemo never appeared: the page did not start');
  await post('/xc/controls', JSON.stringify(window.holofoilDemo.controls()));
  await post('/xc/table', window.holofoilDemo.tableFormat);
  const job = await (await fetch('/xc/job.json')).json();
  const frames = new Uint8Array(await (await fetch('/xc/cards.rgba')).arrayBuffer());
  for (const c of job.cases) {
    const out = window.holofoilDemo.renderCase({
      settings: c.settings, frames, width: job.width, height: job.height, fps: job.fps, count: c.count, half: c.half,
    });
    await post(`/xc/out/${c.name}`, out);
  }
  await post('/xc/done', 'ok');
}

main().catch((error) => post('/xc/error', String(error?.stack ?? error)));
