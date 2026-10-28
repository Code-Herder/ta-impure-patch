/* cob.js -- the mockup's handle on the in-game wiki's COB host (tagpu/ddraw/src/tagpu_wikicob.c), which
   tools/wiki/serve.py runs: what sim.js's ctx.cob opens.

   cobWorld(request, specs) makes a host for the simulation's types (specs[i] {name, pack} or null, in the
   simulation's type order) and returns its world, whose calls are tagpu_wikicob.h's: spawn, call, query, slice,
   remove and origin, each sent through `request` (one synchronous call answering serve.py's JSON), and value,
   resting, hash and notes (each type's quarantine note, "" for none; a type is quarantined when it is made or when
   its script faults) read from the state the last call answered, and top (each type's model height) from the
   host's making. model(unit) is the unit's posed model as tagpu_wikicob_model leaves it
   (pose.js `parse`), null for a unit that runs no script: one `models` request answers every unit until the next
   call, so a page that draws several frames between two ticks asks once. `request` is a synchronous XMLHttpRequest
   to POST /cob in the page (cobPage) and tools/wiki/cobhost.mjs's worker under node.

   Runs in a page (the global WikiCob) and under node (module.exports). */
const WikiCob = (() => {
  const HOW = { now: 0, later: 1, args: 2 };   // WIKICOB_NOW, WIKICOB_LATER, WIKICOB_ARGS
  // pose.js reads the model block: the page's global, node's module
  const WikiPoseRef = () => typeof WikiPose !== 'undefined' ? WikiPose : require('./pose.js');

  function cobWorld(request, specs) {
    const made = request({ op: 'new', types: specs, seed: 1 });
    const id = made.world, tops = made.tops || [];
    let st = made, models = null, parsed = {};
    const take = got => { st = got; models = null; parsed = {}; return got; };
    function model(unit) {
      if (!models) models = request({ op: 'models', world: id }).models;
      if (!(unit in parsed)) {
        const b64 = models[unit];
        parsed[unit] = b64 ? WikiPoseRef().parse(Uint8Array.from(atob(b64), ch => ch.charCodeAt(0))) : null;
      }
      return parsed[unit];
    }
    return {
      get notes() { return st.notes; },
      spawn: (type, facts) => take(request({ op: 'spawn', world: id, type, facts })).unit,
      call: (unit, script, how, args, facts) =>
        take(request({ op: 'call', world: id, unit, script, how: HOW[how], args: args || [], facts })).r,
      slice: facts => { take(request({ op: 'slice', world: id, facts })); },
      remove: unit => { take(request({ op: 'remove', world: id, unit })); },
      activate: (unit, on) => take(request({ op: 'activate', world: id, unit, on: on ? 1 : 0 })).r,
      // tagpu_wikicob_query: { r, io }, io the four words the script left
      query: (unit, script, io, facts) => {
        const got = take(request({ op: 'query', world: id, unit, script, io, facts }));
        return { r: got.r, io: got.io };
      },
      // tagpu_wikicob_origin: { r, turned, base }, 16.16 words in the model's axes; it changes no state
      origin: (unit, piece) => request({ op: 'origin', world: id, unit, piece }),
      top: type => tops[type] | 0,
      value: (unit, k) => { const v = st.values[unit]; return v ? v[k] : 0; },
      resting: unit => st.resting.includes(unit),
      // tagpu_wikicob_hash from the FNV-1a offset basis, as two uint32 halves
      hash: () => [parseInt(st.hash.slice(0, 8), 16), parseInt(st.hash.slice(8), 16)],
      model,
      free: () => request({ op: 'free', world: id }),
    };
  }

  // the page's request: synchronous, so sim.js's calls answer before they return as the DLL's do
  function pageRequest(url) {
    return req => {
      const x = new XMLHttpRequest();
      x.open('POST', url, false);
      x.setRequestHeader('Content-Type', 'application/json');
      try { x.send(JSON.stringify(req)); } catch (e) { throw new Error(`the COB host is not reachable at ${url}: ${e.message}`); }
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
