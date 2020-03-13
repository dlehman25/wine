#!/usr/bin/perl

# ./zones.pl wine/dlls/tzres/tzres.rc > zones.c
my $tzresname = shift or die("usage: $0 wine/dlls/tzres/tzres.rc\n");

my $displayName;
my %mapping;
my %olsonIDs;
my %tzres;

open FH, "<windowsZones.xml";
while (<FH>)
{
    if (/<!-- *(\(UTC.*) *-->/)
    {
        $displayName = $1;
        $displayName =~ s/^\s+|\s+$//g;
    }
    elsif (/<mapZone other="(.*)" territory="001" type="(.*)"\/>/)
    {
        $windowsID = $1;
        $windowsID =~ s/^\s+|\s+$//g;
        $mapping{$windowsID} = $displayName;

        $olsonIDs{$windowsID} = $2;
    }
}
close(FH);

open(FH, "<$tzresname");
while (<FH>)
{
    if (/^\s+([0-9]+)\s+"([^"]+)"$/)
    {
        $tzres{$2} = $1;
    }
}
close(FH);

print <<EOM
static const tzdata zoneinfo[] =
{
EOM
;

foreach my $windowsID (sort { "\L$a" cmp "\L$b" } keys %mapping)
{
    my $tzid = $tzres{$windowsID} + 0;
    if (!$tzid)
    {
        my $sha = qx(echo -n '$windowsID'| sha1sum);
        chomp($sha);
        $sha1 = substr($sha, 37, 3); # dcb282412af7d2090f3dfb7a16048cc77bc8b92a -> 92a
        $sha1 = "${sha1}0";
        $tzid = hex($sha1);
    }
    printf("    { %-40s %-30s %5u }, /* %-40s */\n",
        "\"$windowsID\",", "\"$olsonIDs{$windowsID}\",", $tzid, $mapping{$windowsID});
}

print <<EOM
    { NULL, NULL, 0 }
};
EOM
