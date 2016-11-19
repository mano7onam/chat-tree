# ChatTreeThatHaveStructureOfTreeAndWhenSomethingBecomeWrongItWillBeWorkingNoMatterWhatHappend
No need in description

./ChatTree -m 5000 -l 10 
./ChatTree -m 5001 -l 30 -p localhost:5000
./ChatTree -m 5002 -l 10 -p localhost:5001
./ChatTree -m 5003 -l 5 -p localhost:5000
./ChatTree -m 5004 -l 5 -p localhost:5003
./ChatTree -m 5005 -l 95 -p localhost:5003
