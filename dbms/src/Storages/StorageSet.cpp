#include <DB/Storages/StorageSet.h>
#include <DB/IO/WriteBufferFromFile.h>
#include <DB/IO/ReadBufferFromFile.h>
#include <DB/IO/CompressedWriteBuffer.h>
#include <DB/IO/CompressedReadBuffer.h>
#include <DB/DataStreams/NativeBlockOutputStream.h>
#include <DB/DataStreams/NativeBlockInputStream.h>
#include <DB/Common/escapeForFileName.h>


namespace DB
{


class SetBlockOutputStream : public IBlockOutputStream
{
public:
	SetBlockOutputStream(SetPtr & set_, const String & backup_path_, const String & backup_tmp_path_, const String & backup_file_name_)
		: set(set_),
		backup_path(backup_path_), backup_tmp_path(backup_tmp_path_),
		backup_file_name(backup_file_name_),
		backup_buf(backup_tmp_path + backup_file_name),
		compressed_backup_buf(backup_buf),
		backup_stream(compressed_backup_buf)
	{
	}

	void write(const Block & block) override
	{
		set->insertFromBlock(block);
		backup_stream.write(block);
	}

	void writeSuffix() override
	{
		backup_stream.flush();
		compressed_backup_buf.next();
		backup_buf.next();

		Poco::File(backup_tmp_path + backup_file_name).renameTo(backup_path + backup_file_name);
	}

private:
	SetPtr set;
	String backup_path;
	String backup_tmp_path;
	String backup_file_name;
	WriteBufferFromFile backup_buf;
	CompressedWriteBuffer compressed_backup_buf;
	NativeBlockOutputStream backup_stream;
};


BlockOutputStreamPtr StorageSet::write(ASTPtr query)
{
	++increment;
	return new SetBlockOutputStream(set, path, path + "tmp/", toString(increment) + ".bin");
}


StorageSet::StorageSet(
	const String & path_,
	const String & name_,
	NamesAndTypesListPtr columns_,
	const NamesAndTypesList & materialized_columns_,
	const NamesAndTypesList & alias_columns_,
	const ColumnDefaults & column_defaults_)
	: IStorage{materialized_columns_, alias_columns_, column_defaults_},
	path(path_ + escapeForFileName(name_) + '/'), name(name_), columns(columns_)
{
	restore();
}


void StorageSet::restore()
{
	Poco::File tmp_dir(path + "tmp/");
	if (!tmp_dir.exists())
	{
		tmp_dir.createDirectories();
		return;
	}

	constexpr auto file_suffix = ".bin";
	constexpr auto file_suffix_size = strlen(file_suffix);

	DataTypeFactory data_type_factory;

	Poco::DirectoryIterator dir_end;
	for (Poco::DirectoryIterator dir_it(path); dir_end != dir_it; ++dir_it)
	{
		const auto & name = dir_it.name();

		if (dir_it->isFile()
			&& name.size() > file_suffix_size
			&& 0 == name.compare(name.size() - file_suffix_size, file_suffix_size, file_suffix)
			&& dir_it->getSize() > 0)
		{
			/// Вычисляем максимальный номер имеющихся файлов с бэкапом, чтобы добавлять следующие файлы с большими номерами.
			UInt64 file_num = parse<UInt64>(name.substr(0, name.size() - file_suffix_size));
			if (file_num > increment)
				increment = file_num;

			restoreFromFile(dir_it->path(), data_type_factory);
		}
	}
}


void StorageSet::restoreFromFile(const String & file_path, const DataTypeFactory & data_type_factory)
{
	ReadBufferFromFile backup_buf(file_path);
	CompressedReadBuffer compressed_backup_buf(backup_buf);
	NativeBlockInputStream backup_stream(compressed_backup_buf, data_type_factory);

	backup_stream.readPrefix();
	while (Block block = backup_stream.read())
		set->insertFromBlock(block);
	backup_stream.readSuffix();

	/// TODO Добавить скорость, сжатые байты, объём данных в памяти, коэффициент сжатия... Обобщить всё логгирование статистики в проекте.
	LOG_INFO(&Logger::get("StorageSet"), std::fixed << std::setprecision(2)
		<< "Loaded from backup file " << file_path << ". "
		<< backup_stream.getInfo().rows << " rows, "
		<< backup_stream.getInfo().bytes / 1048576.0 << " MiB. "
		<< "Set has " << set->getTotalRowCount() << " unique rows.");
}


void StorageSet::rename(const String & new_path_to_db, const String & new_database_name, const String & new_table_name)
{
	/// Переименовываем директорию с данными.
	String new_path = new_path_to_db + escapeForFileName(new_table_name);
	Poco::File(path).renameTo(new_path);

	path = new_path + "/";
	name = new_table_name;
}


}
