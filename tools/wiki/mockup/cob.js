/* cob.js -- the mockup's handle on the in-game wiki's COB host (tagpu/ddraw/src/tagpu_wikicob.c), which
   tools/wiki/serve.py runs: what sim.js's ctx.cob opens.

   cobWorld(request, specs) makes a host for the simulation's types (specs[i] {name, pack} or null, in the
   simulation's type order) and returns its world, whose calls are tagpu_wikicob.h's: spawn, call, slice, remove,
   each sent through `request` (one synchronous call answering serve.py's JSON), and value, resting, hash and notes
   read from the state the last call answered. `request` is a synchronous XMLHttpRequest to POST /cob in the page
   (cobPage) and tools/wiki/cobhost.mjs's worker under node.

   Runs in a page (the global WikiCob) and under node (module.exports). */
const WikiCob = (() => {
  const HOW = { now: 0, later: 1, args: 2 };   // WIKICOB_NOW, WIKICOB_LATER, WIKICOB_ARGS

  function cobWorld(request, specs) {
    const made = request({ op: 'new', types: specs, seed: 1 });
    const id = made.world;
    let st = made;
    const take = got => { st = got; return got; };
    return {
      notes: made.notes,
      spawn: (type, facts) => take(request({ op: 'spawn', world: id, type, facts })).unit,
      call: (unit, script, how, args, facts) =>
        take(request({ op: 'call', world: id, unit, script, how: HOW[how], args: args || [], facts })).r,
      slice: facts => { take(request({ op: 'slice', world: id, facts })); },
      remove: unit => { take(request({ op: 'remove', world: id, unit })); },
      activate: (unit, on) => take(request({ op: 'activate', world: id, unit, on: on ? 1 : 0 })).r,
      value: (unit, k) => { const v = st.values[unit]; return v ? v[k] : 0; },
      resting: unit => st.resting.includes(unit),
      // tagpu_wikicob_hash from the FNV-1a offset basis, as two uint32 halves
      hash: () => [parseInt(st.hash.slice(0, 8), 16), parseInt(st.hash.slice(8), 16)],
      free: () => request({ op: 'free', world: id }),
    };
  }

  // the page's request: synchronous, so sim.js's calls answer before they return as the DLL's do
  function pageRequest(url) {
    return req => {
      const x = new XMLHttpRequest();
      x.open('POST', url, false);
      x.setRequestHeader('Content-Type', 'application/json');
      x.send(JSON.stringify(req));
      if (x.status !== 200) throw new Error(`the COB host answered ${x.status}`);
      const got = JSON.parse(x.responseText);
      if (got.error) throw new Error(`the COB host: ${got.error}`);
      return got;
    };
  }

  // what the page hands sim.js as ctx.cob: scripts from `pack`, a gfx pack folder under the repository
  function cobPage(pack, url = '/cob') {
    const request = pageRequest(url);
    return { open: names => cobWorld(request, names.map(n => n && { name: n, pack })) };
  }

  return { cobWorld, cobPage, HOW };
})();
if (typeof module !== 'undefined') module.exports = WikiCob;
