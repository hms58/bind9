#!/usr/bin/perl
#
# Copyright (C) 1999, 2000  Internet Software Consortium.
# 
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
# 
# THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
# ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
# OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
# CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
# DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
# PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
# ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
# SOFTWARE.

# Compare two files, each with the output from dig, for differences.
# Ignore "unimportant" differences, like ordering of NS lines, TTL's,
# etc...

$file1 = $ARGV[0];
$file2 = $ARGV[1];

$count = 0;
$firstname = "";
$status = 0;

open(FILE1, $file1) || die("$! $file1");
while (<FILE1>) {
  chomp;
  next if (/^;/);
  if (/^(\S+)\s+\S+\s+(\S+)\s+(\S+)\s+(.+)$/) {
    $name = $1;
    $class = $2;
    $type = $3;
    $value = $4;
    if ($type eq "SOA") {
      $firstname = $name if ($firstname eq "");
      if ($name eq $firstname) {
	$name = "$name$count";
	$count++;
      }
    }
    if ($entry{"$name ; $class.$type ; $value"} ne "") {
      $line = $entry{"$name ; $class.$type ; $value"};
      print ("Duplicate entry in $file1:\n> $_\n< $line\n");
    } else {
      $entry{"$name ; $class.$type ; $value"} = $_;
    }
  }
}
close (FILE1);

$printed = 0;

open(FILE2, $file2) || die("$! $file2");
while (<FILE2>) {
  chomp;
  next if (/^;/);
  if (/^(\S+)\s+\S+\s+(\S+)\s+(\S+)\s+(.+)$/) {
    $name = $1;
    $class = $2;
    $type = $3;
    $value = $4;
    if (($name eq $firstname) && ($type eq "SOA")) {
      $count--;
      $name = "$name$count";
    }
    if ($entry{"$name ; $class.$type ; $value"} ne "") {
      $entry{"$name ; $class.$type ; $value"} = "";
    } else {
      print ("Only in $file2 (missing from $file1):\n") if ($printed == 0);
      print ("> $_\n");
      $printed++;
      $status = 1;
    }
  }
}
close (FILE2);

$printed = 0;

foreach $key (keys(%entry)) {
  if ($entry{$key} ne "") {
    print ("Only in $file1 (missing from $file2):\n") if ($printed == 0);
    print ("< $entry{$key}\n");
    $status = 1;
    $printed++;
  }
}

exit($status);
