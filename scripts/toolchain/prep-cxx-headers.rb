#!/usr/bin/ruby
# extract and istall the C++ freestanding header set

require 'fileutils'
include FileUtils

# headers defined in the C++ freestading environment
# - excludes because of rtti requirement:
#   - 'thread', 'exception', 'typeinfo'
# - excludes because of exception support requirement:
#   - 'new'
HEADERS = %w[cstdint cstddef cstdlib limits initializer_list cstdarg type_traits
             atomic]

# headers provided by the freestanding C standard
PROVIDED = %w[stdint.h stddef.h stdarg.h stdbool.h]

# mappings for headers with known differences in source / install location
ALIAS = { 
  'bits/c++config.h'               => 'bits/c++config',
  'bits/os_defines.h'              => 'generic/os_defines.h',
  'bits/cpu_defines.h'             => 'generic/cpu_defines.h',
  'bits/atomic_lockfree_defines.h' => 'libsupc++/atomic_lockfree_defines.h',
}

# c++config settings (since we're not compiling libstdc++v3 we need this)
CONFIG = {
  :set => {
    '_GLIBCXX_HAVE_ATTRIBUTE_VISIBILITY' => '1',
    '_GLIBCXX_EXTERN_TEMPLATE'           => '1',
    '_GLIBCXX_INLINE_VERSION'            => '0',
  },
  :add => {
    '_GLIBCXX_HAVE_AS_SYMVER_DIRECTIVE'  => '1',
    '_GLIBCXX_HAVE_AT_QUICK_EXIT'        => '0',
    '_GLIBCXX_HAVE_INT64_T'              => '1',
    '_GLIBCXX_HAVE_INT64_T_LONG'         => '1',
    '_GLIBCXX_HAVE_QUICK_EXIT'           => '0',
    '_GLIBCXX_HAVE_STDBOOL_H'            => '1',
    '_GLIBCXX_HAVE_STDINT_H'             => '1',
    '_GLIBCXX_ATOMIC_BUILTINS'           => '1',
    '_GLIBCXX_HOSTED'                    => '0',
    '_GLIBCXX_USE_INT128'                => '1',
    '_GLIBCXX_USE_LONG_LONG'             => '1',
  }
}

def search_header( source_root, header )
  location = nil
  depends  = []

  if ALIAS.has_key? header
    puts "-> subst #{ALIAS[header]} => #{header}"
    header   = ALIAS[header] if ALIAS.has_key? header
  end

  Dir.glob( "#{source_root}/**/#{header}" ).each do |match|
    unless File.directory? match
      location = match
      if location =~ /\/(tr\d)\//
        puts "Ignoring #{$1} version of #{header}.."
        next
      end

      File.open( match ).grep( /^\s*#\s*include\s*<[^>]+>/ ) do |line|
        line.gsub!( /^\s*#\s*include\s*<([^>\n]+)>/, '\1' )
        depends << line.strip unless depends.include? line
      end

      puts "Found instance of #{header} (#{File.dirname( match )})"
      depends.each { |dep| puts "-> depends on #{dep}" }

      return [location, depends]
    end
  end
  raise "Couldn't find any instance of #{header}!"
end

def search_all( source_root )
  located = {}
  required = HEADERS
  until required.empty? do
    headers = []
    required.each do |header|
      location, depends = search_header( source_root, header )

      located[header] = location
      headers += depends.select { |dep| !PROVIDED.include?( dep ) && 
                                        !located.has_key?( dep ) }
    end
    required = headers
  end

  located
end

def do_install( headers, sys_root )
  install_root = "#{sys_root}/include/c++"

  puts "Installing C++11 freestanding headers to #{install_root}"
  makedirs install_root
  headers.each do |dest, src|
    install_path = "#{install_root}/#{dest}"

    if not File.directory? File.dirname( install_path )
      makedirs File.dirname( "#{install_path}" )
    end

    if dest == 'bits/c++config.h'
      # needs manual patching since libstdc++ was not actually compiled.
      puts "Updating #{dest}.."
      open( src, 'r' ) do |input|
        open( install_path, 'w' ) do |out|
          data = input.read
          CONFIG[:set].each do |key, val|
            data.gsub!( /^\s*#\s*define\s+#{key}\s*$/, "# define #{key} #{val}" )
          end

          out << data
          out << "// generated by #{File.basename( __FILE__ )}\n\n"

          CONFIG[:add].each do |key, val|
            out << "# define #{key} #{val}\n"
          end
        out << "\n#endif //_GLIBCXX_CXX_CONFIG_H\n"
        end
      end
    else
      cp src, install_path
    end
  end
end

if ARGV.count != 2
  $stderr.write( "usage: #{File.basename( __FILE__ )} prefix source_root\n" )
  exit 1
else
  prefix, source_root = ARGV
  headers = search_all( source_root )
  do_install( headers, prefix )
end
