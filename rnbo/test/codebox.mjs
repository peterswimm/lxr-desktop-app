// Runs RNBO codebox~ code in Node, for testing.
//
// Translates the subset of the codebox language the LXR code uses into JavaScript:
// @param / @state declarations, typed parameters (c: list), FixedDoubleArray, the
// math built-ins and samplerate(). Top-level statements that are not declarations or
// functions form the per-sample body, as in codebox~.
//
// instantiateMessage() runs a message-domain codebox: the body runs for messages to
// the first inlet (listin1), functions named inN handle the other inlets, and every
// assignment to outN / listoutN is recorded as an output event.

const BUILTINS = `
const trunc = Math.trunc, floor = Math.floor, abs = Math.abs, min = Math.min, max = Math.max,
	pow = Math.pow, log2 = Math.log2, log = Math.log, exp = Math.exp, sqrt = Math.sqrt,
	isnan = Number.isNaN, round = Math.round;
function samplerate() { return __sr; }
// list.store-like slots of number lists; lookup returns a copy
class liststore {
	constructor(o) {
		this.max = o.maxlistsize;
		this.slots = Array.from({ length: o.slotcount }, () => []);
	}
	store(i, l) { if (i >= 0 && i < this.slots.length) this.slots[i] = l.slice(0, this.max); }
	lookup(i) { return i >= 0 && i < this.slots.length ? this.slots[i].slice() : []; }
	clear() { this.slots = this.slots.map(() => []); }
}
`;

function stripComments(src) {
	let out = '';
	let i = 0;
	while (i < src.length) {
		const c = src[i];
		if (c === '"') {
			const j = src.indexOf('"', i + 1);
			out += src.slice(i, j + 1);
			i = j + 1;
		} else if (src.startsWith('//', i)) {
			while (i < src.length && src[i] !== '\n') i++;
		} else if (src.startsWith('/*', i)) {
			i = src.indexOf('*/', i + 2) + 2;
		} else {
			out += c;
			i++;
		}
	}
	return out;
}

export function transpile(src, message = false) {
	src = stripComments(src);
	if (message) {
		// outputs are events in a message codebox
		src = src.replace(/\b(?:list)?out(\d+)\s*=(?!=)\s*([^;]+);/g, '__emit($1, $2);');
	}
	const params = [];
	const decls = [];
	const funcs = [];
	const body = [];
	let mode = null;
	let depth = 0;
	for (let line of src.split('\n')) {
		const t = line.trim();
		if (!t) continue;
		if (depth === 0) {
			if (t.startsWith('function ')) mode = funcs;
			else if (t.startsWith('@param')) {
				const m = t.match(/^@param\((\{.*\})\)\s+(\w+)\s*=\s*([^;]+);$/);
				if (!m) throw new Error('cannot parse: ' + t);
				params.push({ name: m[2], init: Number(m[3]), opts: m[1] });
				decls.push(`var ${m[2]} = ${m[3]};`);
				continue;
			} else if (t.startsWith('@state')) {
				let m = t.match(/^@state\s+(\w+)\s*=\s*new\s+Fixed(?:Double|Float|Int|Uint8|Uint16|Uint32)Array\((\d+)\);$/);
				if (m) {
					decls.push(`var ${m[1]} = new Float64Array(${m[2]});`);
					continue;
				}
				m = t.match(/^@state\s+(\w+)(?:\s*:\s*\w+)?\s*=\s*([^;]+);$/);
				if (!m) throw new Error('cannot parse: ' + t);
				decls.push(`var ${m[1]} = ${m[2]};`);
				continue;
			} else if (t.startsWith('const ')) {
				decls.push(t);
				continue;
			} else mode = body;
		}
		// type annotations: "c: list", "x: number", "let i: Index", "function f(): list"
		line = line.replace(/(\w)\s*:\s*(list|number|Int|Index|auto)\b/g, '$1');
		line = line.replace(/\)\s*:\s*(list|number|Int|Index|auto)\s*\{/g, ') {');
		mode.push(line);
		for (const c of t) {
			if (c === '{') depth++;
			else if (c === '}') depth--;
		}
	}
	if (message) return messageModule(params, decls, funcs, body, src);
	const byNumber = (a, b) => Number(a.replace(/\D/g, '')) - Number(b.replace(/\D/g, ''));
	const ins = [...new Set(src.match(/\bin\d+\b/g) || [])].sort(byNumber);
	const outs = [...new Set(src.match(/\bout\d+\b/g) || [])].sort(byNumber);
	const js = [
		BUILTINS,
		`var ${[...ins, ...outs].map((n) => n + ' = 0').join(', ')};`,
		...decls,
		...funcs,
		'function __perform() {',
		...body,
		'}',
		'function __set(n, v) { switch (n) {',
		...params.map((p) => `case ${JSON.stringify(p.name)}: ${p.name} = v; break;`),
		'default: throw new Error("no param " + n); } }',
		'function __get(n) { return eval(n); }',
		`function __in(i, v) { switch (i) { ${ins.map((n) => `case ${n.slice(2)}: ${n} = v; break;`).join(' ')} } }`,
		`function __out() { return [${outs.join(', ')}]; }`,
		'return { dspsetup, perform: __perform, set: __set, get: __get, input: __in, output: __out };',
	].join('\n');
	return { js, params, ins, outs };
}

function messageModule(params, decls, funcs, body, src) {
	const handlers = [...src.matchAll(/function in(\d+)\s*\(/g)].map((m) => Number(m[1]));
	const js = [
		BUILTINS,
		'var listin1 = [];',
		'var __events = [];',
		'function __emit(n, v) { __events.push([n, Array.isArray(v) ? v.slice() : v]); }',
		...decls,
		...funcs,
		'function __body() {',
		...body,
		'}',
		'function __set(n, v) { switch (n) {',
		...params.map((p) => `case ${JSON.stringify(p.name)}: ${p.name} = v; break;`),
		'default: throw new Error("no param " + n); } }',
		'function __get(n) { return eval(n); }',
		'function __in(i, v) { if (i == 1) { listin1 = v; __body(); return; } switch (i) {',
		...handlers.map((n) => `case ${n}: in${n}(v); break;`),
		'default: throw new Error("no inlet " + i); } }',
		'function __take() { const e = __events; __events = []; return e; }',
		'function __call(n, ...a) { return eval(n)(...a); }',
		'function __setv(n, v) { eval(n + " = v"); }',
		'return { set: __set, get: __get, input: __in, take: __take, call: __call, setv: __setv };',
	].join('\n');
	return { js, params, handlers };
}

export function instantiateMessage(src) {
	const { js, params, handlers } = transpile(src, true);
	const box = new Function('__sr', js)(44100);
	return { box, params, handlers };
}

export function instantiate(src, sampleRate) {
	const { js, params, ins, outs } = transpile(src);
	const make = new Function('__sr', js);
	const dsp = make(sampleRate);
	dsp.dspsetup();
	return { dsp, params, ins, outs };
}
