all: README apt-cacher-ng.pdf man/acngfs.8 man/apt-cacher-ng.8 html/index.html

html/.gitignore:
	mkdir -p html
	touch $@

man/.gitignore:
	mkdir -p man
	touch $@

README: src/README.but
	halibut --text=README src/textparm.but src/README.but

apt-cacher-ng.pdf: src/README.but
	halibut --pdf=$@ src/README.but

man/apt-cacher-ng.8: src/manpage.but
	halibut --man=$@ src/manpage.but

man/acngfs.8: src/acngfs.but
	halibut --man=$@ src/acngfs.but

html/index.html: src/README.but
	cd html && halibut --html ../src/README.but
