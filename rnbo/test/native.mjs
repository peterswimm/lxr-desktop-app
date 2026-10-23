// Builds native/test/lxr_native: the C++ generated from the codebox sources
// (native/tools/codebox2cpp.py), driven by the same scripts as the firmware references.
// run.mjs and seqref.mjs use it with --native.
import { execFileSync } from 'child_process';
import { mkdirSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const HERE = dirname(fileURLToPath(import.meta.url));
const NATIVE = join(HERE, '../../native');
export const NATIVE_BIN = join(NATIVE, 'build/test/lxr_native');

export function buildNative() {
	execFileSync('python3', [join(NATIVE, 'tools/codebox2cpp.py')], { stdio: ['ignore', 'ignore', 'inherit'] });
	mkdirSync(dirname(NATIVE_BIN), { recursive: true });
	execFileSync(process.env.CXX || 'g++', ['-std=c++17', '-O2', '-ffp-contract=off', '-fno-fast-math',
		'-I', join(NATIVE, 'core'), '-I', join(NATIVE, 'generated'),
		join(NATIVE, 'test/lxr_native.cpp'), '-o', NATIVE_BIN], { stdio: 'inherit' });
	return NATIVE_BIN;
}
