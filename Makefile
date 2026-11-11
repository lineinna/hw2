# Your NTU student ID, e.g. b12902033. Set it here once, or pass it as
# `make submission STUDENT_ID=b12902033`.
STUDENT_ID =

# The files you submit besides this Makefile, which is always included. List
# every source and header that sp is built from.
SUBMISSION_FILES = sp.c sp.h

# Build sp. Change this rule and `clean` as you like, for example to compile
# more sources or add flags.
sp: sp.c sp.h
	gcc -o sp sp.c

.PHONY: clean
clean:
	rm -f sp

# $(call copy_submission,DIR) is a shell command that copies this Makefile and
# SUBMISSION_FILES into DIR, keeping their relative paths. On a bad entry it
# prints why and exits the shell, so callers clean up in an EXIT trap.
copy_submission = \
	for file in Makefile $(SUBMISSION_FILES); do \
		case "$$file" in \
		/* | .. | ../* | */.. | */../*) \
			echo "make $@: $$file in SUBMISSION_FILES is not a path inside this directory" >&2; \
			exit 1 ;; \
		esac; \
		if [ ! -f "$$file" ]; then \
			echo "make $@: $$file is listed in SUBMISSION_FILES but is missing or not a regular file" >&2; \
			exit 1; \
		fi; \
		mkdir -p "$(1)/$$(dirname "$$file")" && cp "$$file" "$(1)/$$file" || exit 1; \
	done

# Grade exactly what `make submission` would submit, copied into a temp dir.
.PHONY: test
test: sp
	@platform=$$(judge/bin/detect-platform.sh) || exit $$?; \
	bindir="judge/bin/$$platform"; \
	if [ ! -x "$$bindir/judge" ]; then \
		echo "make test: no judge build published for platform $$platform" >&2; \
		exit 1; \
	fi; \
	work=$$(mktemp -d) || exit 1; \
	trap 'rm -rf "$$work"' EXIT; \
	trap 'exit 1' HUP INT TERM; \
	mkdir "$$work/scratch" || exit 1; \
	$(call copy_submission,$$work/submission); \
	"$$bindir/judge" \
		--plan-root tests/public \
		--submission "$$work/submission" \
		--scratch-root "$$work/scratch" \
		--student-program sp \
		--consultant "$$bindir/consultant" \
		--format human --ui auto

# Pack the Makefile and SUBMISSION_FILES into <student-id>.zip, under a single
# directory named after the student ID.
.PHONY: submission
submission:
	@id=$$(printf '%s' '$(strip $(STUDENT_ID))' | tr '[:upper:]' '[:lower:]'); \
	if [ -z "$$id" ]; then \
		echo "make submission: STUDENT_ID is not set; run" \
			"'make submission STUDENT_ID=b12902033' or set it in the Makefile" >&2; \
		exit 1; \
	fi; \
	if ! printf '%s\n' "$$id" | LC_ALL=C grep -Eqx '[a-z][a-z0-9]{8}'; then \
		echo "make submission: STUDENT_ID '$$id' is not a student ID such as" \
			"b12902033 (a letter followed by 8 letters or digits)" >&2; \
		exit 1; \
	fi; \
	if ! command -v zip >/dev/null; then \
		echo "make submission: zip is not installed (on Ubuntu: sudo apt install zip)" >&2; \
		exit 1; \
	fi; \
	rm -f "$$id.zip"; \
	work=$$(mktemp -d) || exit 1; \
	trap 'rm -rf "$$work"' EXIT; \
	trap 'exit 1' HUP INT TERM; \
	$(call copy_submission,$$work/$$id); \
	(cd "$$work" && zip -X -r "$(CURDIR)/$$id.zip" "$$id") || exit 1; \
	echo "Created $$id.zip. Upload it to NTU COOL."
