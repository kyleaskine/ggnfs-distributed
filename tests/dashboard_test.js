/* dashboard_test.js — checks the client-rollup arithmetic in dashboard.html.
 *
 * The rate column is the one number on that page that has been wrong twice, in
 * opposite directions: first as relations/SUM(seconds) at box level (which
 * reports one core's rate for a 32-core machine), then as a sum of per-worker
 * rates (which multiplies a GPU by its lease-slot count — 582 rel/s displayed
 * for a card measured at 176). The rule is per-device, then summed across
 * devices, and it is easy to break again while editing the page.
 *
 * Reads the functions straight out of dashboard.html so it cannot drift from
 * what ships.  Run:  node tests/dashboard_test.js
 */
'use strict';
const fs = require('fs');
const path = require('path');

const html = fs.readFileSync(
  path.join(__dirname, '..', 'dashboard.html'), 'utf8');

let src = '';
for (const fn of ['clientGroupId', 'clientDeviceId', 'groupClients']) {
  const m = html.match(new RegExp('\\n  function ' + fn + '\\(.*?\\n  \\}\\n', 's'));
  if (!m) { console.error('could not find ' + fn + '() in dashboard.html'); process.exit(2); }
  src += m[0];
}
const { clientDeviceId, groupClients } =
  new Function(src + '\nreturn { clientDeviceId, groupClients };')();

let bad = 0;
function chk(name, got, want) {
  const ok = Math.abs(got - want) < 0.5;
  console.log((ok ? '  ok   ' : '  FAIL ') + name +
              '  got=' + got.toFixed(1) + ' want=' + want.toFixed(1));
  if (!ok) bad++;
}

/* A 32-core box: workers run in parallel, so the box is 32x one core. */
const cpu = [];
for (let i = 0; i < 32; i++)
  cpu.push({ id: 'box-w' + i, relations: 100000, sieve_seconds_total: 34000,
             submissions: 28, 'class': 'cpu' });
{
  const g = groupClients(cpu)[0];
  chk('32-core box = 32x one core', g.rel_per_sec, 32 * (100000 / 34000));
  chk('  workers column counts devices', g.workers.length, 32);
}

/* One card with three lease-slot ids. They serialise on the card, so the box
 * is NOT their sum. Numbers measured on prod. */
{
  const g = groupClients([
    { id: 'K-5070-w0-s0', relations: 173901, sieve_seconds_total: 867, submissions: 36, 'class': 'gpu' },
    { id: 'K-5070-w0-s1', relations: 165993, sieve_seconds_total: 834, submissions: 35, 'class': 'gpu' },
    { id: 'K-5070-w0',    relations:   4266, sieve_seconds_total:  23, submissions:  1, 'class': 'gpu' }
  ])[0];
  chk('one card is not 3x its slots', g.rel_per_sec, 344160 / 1724);
  chk('  workers column = 1 device', g.workers.length, 1);
  chk('  expansion reconciles with the box row', g.workers[0].rel_per_sec, g.rel_per_sec);
  chk('  expanded row carries the card total', g.workers[0].relations, 344160);
}

/* Two cards, two slots each: average within a device, sum across them. */
{
  const two = [];
  for (const w of ['w0', 'w1']) for (const k of [0, 1])
    two.push({ id: 'b-' + w + '-s' + k, relations: 100, sieve_seconds_total: 1,
               submissions: 1, 'class': 'gpu' });
  const g = groupClients(two)[0];
  chk('two cards sum, their slots average', g.rel_per_sec, 200);
  chk('  workers column = 2 devices', g.workers.length, 2);
}

/* A client that has not reported timing yet must not produce NaN. */
chk('zero seconds yields 0, not NaN',
    groupClients([{ id: 'z-w0', relations: 5, sieve_seconds_total: 0, submissions: 1 }])[0].rel_per_sec, 0);

/* client_id is chosen by the client and stored verbatim, so the rollup maps
 * must not be plain objects. */
{
  const before = Object.prototype.relations;
  groupClients([{ id: '__proto__-w0', relations: 9, sieve_seconds_total: 1, submissions: 1 },
                { id: 'constructor',  relations: 9, sieve_seconds_total: 1, submissions: 1 }]);
  const poisoned = Object.prototype.relations !== before || ({}).seconds !== undefined;
  console.log((poisoned ? '  FAIL ' : '  ok   ') +
              'hostile client_id does not reach Object.prototype');
  if (poisoned) bad++;
}

console.log(bad ? '\n' + bad + ' FAILURE(S)' : '\nall dashboard checks pass');
process.exit(bad ? 1 : 0);
