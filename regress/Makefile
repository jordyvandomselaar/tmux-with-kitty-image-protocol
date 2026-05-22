TESTS!= echo *.sh

.PHONY: all $(TESTS) kitty-images
.NOTPARALLEL: all $(TESTS) kitty-images

all: $(TESTS)

$(TESTS):
	sh $*.sh
	sleep 1

kitty-images:
	REQUIRE_KITTY_IMAGES=1 sh kitty-apc.sh
	sleep 1
