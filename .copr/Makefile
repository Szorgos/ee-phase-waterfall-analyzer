# COPR "make srpm" módszer: a forrás és a spec a git-tárolóból készül, a verzió a CMakeLists.txt-ből.
# Az RNNoise betanított modellje (Source1) az Ubuntu forráscsomagjából jön, ellenőrzött sha256-tal.
#   COPR beállítás: Source type = SCM, Clone url = https://github.com/Szorgos/ee-phase-waterfall-analyzer.git,
#   Spec File = packaging/fedora/aqulify.spec, SRPM build method = make srpm
outdir ?= $(CURDIR)
spec ?= packaging/fedora/aqulify.spec

srpm:
	command -v git >/dev/null 2>&1 || dnf -y install git
	command -v rpmbuild >/dev/null 2>&1 || dnf -y install rpm-build
	command -v curl >/dev/null 2>&1 || dnf -y install curl
	set -e; \
	top=$$(git rev-parse --show-toplevel); cd "$$top"; \
	git config --global --add safe.directory "$$top" || true; \
	ver=$$(sed -n 's/^ *VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -1); \
	work=$$(mktemp -d); \
	git archive --format=tar.gz --prefix=aqulify-$$ver/ -o $$work/aqulify-$$ver.tar.gz HEAD; \
	rnn=$$work/rnnoise_0.2+ds.orig.tar.xz; \
	for i in 1 2 3; do curl -fsSL -o $$rnn http://archive.ubuntu.com/ubuntu/pool/universe/r/rnnoise/rnnoise_0.2+ds.orig.tar.xz && break; sleep 15; done; \
	echo "db2718ee4a0b294bfbbe198fe94aabfdc8cd0d77cb9cd95d76c7a818ad805f0b  $$rnn" | sha256sum -c; \
	sed "s/^Version: .*/Version:        $$ver/" $(spec) > $$work/aqulify.spec; \
	rpmbuild -bs --define "_sourcedir $$work" --define "_srcrpmdir $(outdir)" $$work/aqulify.spec
