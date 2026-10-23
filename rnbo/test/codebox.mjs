// Runs RNBO codebox~ code in Node, for testing.
//
// Translates the subset of the codebox language the LXR engine uses into JavaScript:
// @param / @state declarations, typed parameters (c: list), FixedDoubleArray, the
// math built-ins and samplerate(). Top-level statements that are not declarations or
// functions form the per-sample body, as in codebox~.

const BUILTINS = `
const trunc = Math.trunc, floor = Math.floor, abs = Math.abs, min = Math.min, max = Math.max,
	pow = Math.pow, log2 = Math.log2, log = Math.log, exp = Math.exp, sqrt = Math.sqrt,
	isnan = Number.isNaN, round = Math.round;
function samplerate() { return __sr; }
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

export function transpile(src) {
	src = stripComments(src);
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
				let m = t.match(/^@state\s+(\w+)\s*=\s*new\s+Fixed(?:Double|Float|Int)Array\((\d+)\);$/);
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
		// type annotations: "c: list", "x: number", "let i: Index"
		line = line.replace(/(\w)\s*:\s*(list|number|Int|Index|auto)\b/g, '$1');
		mode.push(line);
		for (const c of t) {
			if (c === '{') depth++;
			else if (c === '}') depth--;
		}
	}
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

export function instantiate(src, sampleRate) {
	const { js, params, ins, outs } = transpile(src);
	const make = new Function('__sr', js);
	const dsp = make(sampleRate);
	dsp.dspsetup();
	return { dsp, params, ins, outs };
}
