#!/usr/bin/perl -w

use Digest::MD5 qw(md5 md5_hex);

$netid = $ENV{USER};
$dig = md5_hex($netid);
$dig = substr($dig,16+8);

$port = (hex($dig) % (65536 - 1501))+1500;


print $netid, " -> ", $port, "\n" ;

