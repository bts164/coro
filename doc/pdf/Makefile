SRC      := ../cheatsheet.md
TEMPLATE := cheatsheet.tex
OUT      := ../cheatsheet.pdf

.PHONY: all clean install-deps check-deps

all: check-deps $(OUT)

$(OUT): $(SRC) $(TEMPLATE)
	sed '/^</d' $(SRC) | pandoc \
	  --from=markdown \
	  --template=$(TEMPLATE) \
	  --pdf-engine=xelatex \
	  --highlight-style=tango \
	  --variable=colorlinks=true \
	  -o $(OUT)
	@echo "Generated $(OUT)"

clean:
	rm -f $(OUT) *.aux *.log *.out *.pdf

# ── Dependency checks ──────────────────────────────────────────────────────────

check-deps:
	@command -v pandoc   >/dev/null 2>&1 || { echo "ERROR: pandoc not found. Run 'make install-deps'."; exit 1; }
	@command -v xelatex  >/dev/null 2>&1 || { echo "ERROR: xelatex not found. Run 'make install-deps'."; exit 1; }

install-deps:
	sudo apt-get install -y \
	  pandoc \
	  texlive-xetex \
	  texlive-fonts-recommended \
	  texlive-fonts-extra \
	  texlive-latex-extra
	@echo "Dependencies installed. Run 'make' to build the PDF."
