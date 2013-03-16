SRC=UglyContext.java UglyType.java UglyError.java
CLASSPATH=/usr/share/java/jna.jar

build:
	javac -cp $(CLASSPATH) -d . $(SRC)

test: build
	javac -cp . t/*.java
	./t/test.sh "$(CLASSPATH):.:t" "../c/.libs"

clean:	
	rm -f ugly/*.class
