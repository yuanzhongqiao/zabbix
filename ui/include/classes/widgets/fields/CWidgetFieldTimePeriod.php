<?php declare(strict_types = 0);
/*
** Zabbix
** Copyright (C) 2001-2023 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/


namespace Zabbix\Widgets\Fields;

use CAbsoluteTimeParser,
	CParser,
	CRelativeTimeParser;

use Zabbix\Widgets\CWidgetField;

class CWidgetFieldTimePeriod extends CWidgetField {

	public const DEFAULT_VIEW = \CWidgetFieldTimePeriodView::class;
	public const DEFAULT_VALUE = ['from' => '', 'to' => ''];

	public const DATA_SOURCE_DEFAULT = 0;
	public const DATA_SOURCE_WIDGET = 1;
	public const DATA_SOURCE_DASHBOARD = 2;

	private int $data_source = self::DATA_SOURCE_DEFAULT;

	private bool $is_date_only;

	public function __construct(string $name, string $label = null, bool $is_date_only = false) {
		parent::__construct($name, $label);

		$this->is_date_only = $is_date_only;

		$this
			->setDefault(self::DEFAULT_VALUE)
			->setMaxLength(255);
	}

	public function setValue($value): self {
		unset($value['data_source']);
		$this->value = (array) $value;

		if (array_key_exists('reference', $this->value)) {
			$this->data_source = $this->value['reference'] === 'DASHBOARD'
				? self::DATA_SOURCE_DASHBOARD
				: self::DATA_SOURCE_WIDGET;
		}
		else {
			$this->data_source = self::DATA_SOURCE_DEFAULT;
		}

		return $this;
	}

	public function setFlags(int $flags): self {
		parent::setFlags($flags);

		if (($flags & self::FLAG_NOT_EMPTY) !== 0) {
			$strict_validation_rules = $this->getValidationRules();

			if ($this->data_source === self::DATA_SOURCE_DEFAULT) {
				self::setValidationRuleFlag($strict_validation_rules['fields']['from'], API_NOT_EMPTY);
				self::setValidationRuleFlag($strict_validation_rules['fields']['to'], API_NOT_EMPTY);
			}
			else {
				self::setValidationRuleFlag($strict_validation_rules['fields']['reference'], API_NOT_EMPTY);
			}

			$this->setStrictValidationRules($strict_validation_rules);
		}
		else {
			$this->setStrictValidationRules();
		}

		return $this;
	}

	public function validate(bool $strict = false): array {
		if ($errors = parent::validate($strict)) {
			return $errors;
		}

		$field_value = $this->getValue();

		if ($this->data_source === self::DATA_SOURCE_DEFAULT) {
			$period = ['from' => 0, 'to' => 0];
			$absolute_time_parser = new CAbsoluteTimeParser();
			$relative_time_parser = new CRelativeTimeParser();
			$has_errors = false;

			foreach ($field_value as $name => $value) {
				if ($absolute_time_parser->parse($value) === CParser::PARSE_SUCCESS) {
					$datetime = $absolute_time_parser->getDateTime(true);

					if ($this->is_date_only && $datetime->format('H:i:s') !== '00:00:00') {
						$has_errors = true;
						break;
					}

					$period[$name] = $datetime->getTimestamp();
					continue;
				}

				if ($relative_time_parser->parse($value) === CParser::PARSE_SUCCESS) {
					$datetime = $absolute_time_parser->getDateTime(true);

					if ($this->is_date_only) {
						foreach ($relative_time_parser->getTokens() as $token) {
							if ($token['suffix'] === 'h' || $token['suffix'] === 'm' || $token['suffix'] === 's') {
								$has_errors = true;
								break;
							}
						}
					}

					$period[$name] = $datetime->getTimestamp();
					continue;
				}

				if ($has_errors) {
					break;
				}
			}

			if ($period['from'] !== 0 && $period['to'] !== 0 && $period['from'] >= $period['to']) {
				$has_errors = true;
			}

			if ($has_errors) {
				$this->setValue($this->default);

				return [
					_s('Invalid parameter "%1$s": %2$s.', $this->full_name ?? $this->label ?? $this->name,
						$this->is_date_only ? _('a date is expected') : _('a time is expected')
					)
				];
			}
		}

		return [];
	}

	protected function getValidationRules(): array {
		return ($this->data_source === self::DATA_SOURCE_DEFAULT)
			? ['type' => API_OBJECT, 'fields' => [
				'data_source' => ['type' => API_INT32, 'in' => implode(',', [self::DATA_SOURCE_DEFAULT, self::DATA_SOURCE_WIDGET, self::DATA_SOURCE_DASHBOARD])],
				'from' => ['type' => API_STRING_UTF8, 'flags' => API_REQUIRED, 'length' => $this->getMaxLength()],
				'to' => ['type' => API_STRING_UTF8, 'flags' => API_REQUIRED, 'length' => $this->getMaxLength()]
			]]
			: ['type' => API_OBJECT, 'fields' => [
				'data_source' => ['type' => API_INT32, 'in' => implode(',', [self::DATA_SOURCE_DEFAULT, self::DATA_SOURCE_WIDGET, self::DATA_SOURCE_DASHBOARD])],
				'reference' => ['type' => API_STRING_UTF8, 'flags' => API_REQUIRED]
			]];
	}

	public function toApi(array &$widget_fields = []): void {
		$value = $this->getValue();

		if ($value !== $this->default && is_array($value)) {
			if ($this->data_source === self::DATA_SOURCE_DEFAULT) {
				array_push($widget_fields,
					[
						'type' => ZBX_WIDGET_FIELD_TYPE_STR,
						'name' => $this->name.'[from]',
						'value' => $value['from']
					],
					[
						'type' => ZBX_WIDGET_FIELD_TYPE_STR,
						'name' => $this->name.'[to]',
						'value' => $value['to']
					]
				);
			}
			elseif (array_key_exists('reference', $value)) {
				$widget_fields[] = [
					'type' => ZBX_WIDGET_FIELD_TYPE_STR,
					'name' => $this->name.'[reference]',
					'value' => $value['reference']
				];
			}
		}
	}


	public function getDataSource(): int {
		return $this->data_source;
	}
}
