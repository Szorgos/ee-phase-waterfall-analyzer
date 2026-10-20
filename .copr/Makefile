# COPR "make srpm" módszer: a forrás és a spec a git-tárolóból készül, a verzió a CMakeLists.txt-ből.
#   COPR beállítás: Source type = SCM, Clone url = https://github.com/Szorgos/ee-phase-waterfall-analyzer.git,
#   Spec File = packaging/fedora/aqulify.spec, SRPM build method = make srpm
outdir ?= $(CURDIR)
spec ?= packaging/fedora/aqulify.spec

srpm:
	command -v git >/dev/null 2>&1 || dnf -y install git
	command -v rpmbuild >/dev/null 2>&1 || dnf -y install rpm-build
	set -e; \
	top=$$(git rev-parse --show-toplevel); cd "$$top"; \
	git config --global --add safe.directory "$$top" || true; \
	ver=$$(sed -n 's/^ *VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -1); \
	work=$$(mktemp -d); \
	git archive --format=tar.gz --prefix=aqulify-$$ver/ -o $$work/aqulify-$$ver.tar.gz HEAD; \
	sed "s/^Version: .*/Version:        $$ver/" $(spec) > $$work/aqulify.spec; \
	rpmbuild -bs --define "_sourcedir $$work" --define "_srcrpmdir $(outdir)" $$work/aqulify.spec
