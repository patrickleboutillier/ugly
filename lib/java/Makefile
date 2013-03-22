SRC=UglyRuntime.java UglyLanguage.java UglyValue.java UglyContext.java UglyType.java UglyError.java
CLASSPATH=/usr/share/java/jna.jar

build:
	javac -cp $(CLASSPATH) -d . $(SRC)

clean:	
	rm -f ugly/*.class t/*.class


test: build
	javac -cp . t/*.java
	./t/test.sh "$(CLASSPATH):.:t" "../c/.libs"

check: test

