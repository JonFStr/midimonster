# MIDIMonster development guide

This document serves as a reference for contributors interested in the low-level implementation
of the MIDIMonster. It is currently a work in progress and will be extended as problems come
up and need solving ;)

## Basics

All rules are meant as guidelines. There may be situations where they need to be applied
in spirit rather than by the letter.

### Architectural guidelines

* Change in functionality or behaviour requires a change in documentation.
* There is more honor in deleting code than there is in adding code.
	* Corollary: Code is a liability, not an asset.
	* But: Benchmark the naive implementation before optimizing prematurely.
* The `master` branch must build successfully. Test breaking changes in a branch.
* Commit messages should be in the imperative voice ("When applied, this commit will: ").
* The working language for this repository is english.
* External dependencies are only acceptable when necessary and available from package repositories.
	* Note that external dependencies make OS portability complicated

### Code style

* Tabs for indentations, spaces for word separation
* Lines may not end in spaces or tabs
* There should be no two consecutive spaces (or spaces intermixed with tabs)
* There should be no two consecutive newlines
* All symbol names in `snake_case` except where mandated by external interfaces
* When possible, prefix symbol names with their "namespace" (ie. the relevant section or module name)
* Variables should be appropriately named for what they do
	* The name length should be (positively) correlated with usage
	* Loop counters may be one-character letters
		* Prefer to name unsigned loop counters `u` and signed ones `i`
* Place comments above the section they are commenting on
	* Use inline comments sparingly
* Do not omit '{}' brackets, even if optional (e.g. single-statement conditional bodies)
* Opening braces stay on the same line as the condition

#### C specific

* Prefer lazy designated initializers to `memset()`
* Avoid `atoi()`/`itoa()`, use `strto[u]l[l]()` and `snprintf()`
* Avoid unsafe functions without explicit bounds parameters (eg. `strcat()`). 

# Repository layout

* Keep the root directory as clean as possible
	* Files that are not related directly to the MIDIMonster implementation go into the `assets/` directory
* Prefer vendor-neutral names for configuration files where necessary

# Build pipeline

* The primary build pipeline is `make`

# Architecture

* If there is significant potential for sharing functionality between backends, consider implementing it in `libmmbackend`

# Debugging

